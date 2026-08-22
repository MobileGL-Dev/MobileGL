// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/NonCoreImageFormatScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - AN IMAGE FORMAT GLSL ES CANNOT SPELL.
//
// GL 4.2 has forty image formats; GLSL ES core has thirteen, and GL_NV_image_formats - the only
// thing that adds the rest - is advertised by none of Adreno 830, Mali-G1-Ultra MC12 or
// Mali-G925-Immortalis MC12. A shader that declares one of the other twenty-six therefore has no
// legal ESSL at all: SPIRV-Cross throws for some of them and the driver rejects the token for the
// rest ("'rg32f' : not a legal layout qualifier id"), and dropping the qualifier is refused too
// ("all images have to define layout format"). glBindImageTexture will not take the narrow format
// either - GL_INVALID_VALUE for nineteen of the twenty-six on Adreno, twenty-five on both Malis.
// The stage is lost, the program is "linked but not drawable", and every dispatch silently does
// nothing: KHR-GL43.shader_image_load_store.basic-allFormats-*, single-byte_data_alignment and
// multiple-uniforms are all that one defect.
//
// Espryt emulates the seventeen formats that have a core format of the SAME per-channel width by
// CHANNEL WIDENING - rg32f is carried in an rgba32f, r8ui in an rgba8ui - moving all three layers
// together (ES texture storage, the glBindImageTexture argument, and the shader declaration plus a
// mask on every access). What makes the emulation EXACT rather than approximate is that GL already
// defines the channels a narrow format does not have:
//
//   * imageLoad on a one-channel format returns (r, 0, 0, 1), on a two-channel one (r, g, 0, 1);
//   * imageStore drops the components the format does not have;
//   * a sampler reads the same (r, g, 0, 1).
//
// so the carrier's surplus channels are not free storage - they hold values GL has already named.
//
// EVERY CASE HERE IS PHRASED IN THOSE GL RULES AND NOTHING ELSE, which is what makes it a
// falsifiable net rather than a restatement of the implementation. Magma needs none of the
// machinery (Vulkan takes the declared format natively), a driver that DOES advertise
// GL_NV_image_formats - Mesa's, which is what the software lanes run - keeps the narrow format and
// widens nothing, and all of them must produce the same numbers. A widening that forgot to mask a
// store, or masked it with the wrong constants, or widened the storage without widening the bind,
// fails these on the device while the software lanes stay green.

#include <algorithm>
#include <cmath>
#include <cstring>
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
        // The narrow image is unit 0 and the wide one unit 1; both declare their binding, so this
        // scenario turns on the FORMAT alone and shares nothing with the unit bake
        // ImageFormatQualifierScenario covers.
        constexpr GLuint kNarrowUnit = 0;
        constexpr GLuint kWideUnit = 1;

        class NonCoreImageFormatScenario : public ScenarioTest {
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
                return maxImageUnits > static_cast<GLint>(kWideUnit) && maxComputeImageUniforms >= 2;
            }

            GLuint MakeComputeProgram(const std::string& source) {
                const GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
                const char* text = source.c_str();
                glShaderSource(shader, 1, &text, nullptr);
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

            // Immutable storage, NEAREST filtering and a single level, so the texture is complete
            // for texelFetch as well as image-bindable. `seed` fills every texel of every channel
            // with a value no dispatch writes, so "the store never happened" and "the store wrote
            // the right thing" cannot be confused - which matters here more than usual, because
            // the failure this scenario exists for is a dispatch that silently does nothing.
            GLuint MakeTexture(GLenum internalFormat, GLenum uploadFormat, GLenum uploadType,
                               const void* seed) {
                GLuint texture = 0;
                glGenTextures(1, &texture);
                m_textures.push_back(texture);
                glBindTexture(GL_TEXTURE_2D, texture);
                glTexStorage2D(GL_TEXTURE_2D, 1, internalFormat, kExtent, kExtent);
                if (const GLenum error = FirstGLError()) {
                    ADD_FAILURE() << "allocating storage errored with " << GLErrorName(error);
                    return 0;
                }
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                if (seed != nullptr) {
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kExtent, kExtent, uploadFormat, uploadType, seed);
                }
                while (glGetError() != GL_NO_ERROR) {
                }
                return texture;
            }

            void BindImage(GLuint unit, GLuint texture, GLenum internalFormat, GLenum access) {
                glBindImageTexture(unit, texture, 0, GL_FALSE, 0, access, internalFormat);
                ASSERT_EQ(FirstGLError(), 0u)
                    << "glBindImageTexture refused format " << std::hex << internalFormat;
            }

            void Dispatch(GLuint program) {
                glUseProgram(program);
                glDispatchCompute(kExtent, kExtent, 1);
                glMemoryBarrier(GL_ALL_BARRIER_BITS);
                EXPECT_EQ(FirstGLError(), 0u) << "the dispatch leaked a GL error";
                glUseProgram(0);
            }

            std::vector<GLuint> m_programs;
            std::vector<GLuint> m_textures;

            std::vector<float> ReadFloats(GLuint texture, GLenum format, int componentsPerTexel) {
                std::vector<float> texels(static_cast<std::size_t>(kExtent) * kExtent * componentsPerTexel,
                                          -12345.0f);
                glBindTexture(GL_TEXTURE_2D, texture);
                glGetTexImage(GL_TEXTURE_2D, 0, format, GL_FLOAT, texels.data());
                if (const GLenum error = FirstGLError()) {
                    ADD_FAILURE() << "reading the image back errored with " << GLErrorName(error);
                }
                return texels;
            }

            // A GL_TEXTURE_CUBE_MAP_ARRAY of `cubeCount` cubes, i.e. 6 * cubeCount layer-faces
            // addressed as array layers. The target the allTargets walkers reach last and the one
            // that has caught the most emulation bugs, because it is the only one whose ES
            // equivalent is a 2D array with a different addressing rule from the GL name.
            GLuint MakeCubeArrayTexture(GLenum internalFormat, GLenum uploadFormat, GLenum uploadType,
                                        const void* seed, int cubeCount) {
                GLuint texture = 0;
                glGenTextures(1, &texture);
                m_textures.push_back(texture);
                glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, texture);
                glTexStorage3D(GL_TEXTURE_CUBE_MAP_ARRAY, 1, internalFormat, kExtent, kExtent,
                               6 * cubeCount);
                if (const GLenum error = FirstGLError()) {
                    ADD_FAILURE() << "allocating cube-array storage errored with " << GLErrorName(error);
                    return 0;
                }
                glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                if (seed != nullptr) {
                    glTexSubImage3D(GL_TEXTURE_CUBE_MAP_ARRAY, 0, 0, 0, 0, kExtent, kExtent,
                                    6 * cubeCount, uploadFormat, uploadType, seed);
                }
                while (glGetError() != GL_NO_ERROR) {
                }
                return texture;
            }

            void BindLayeredImage(GLuint unit, GLuint texture, GLenum internalFormat, GLenum access) {
                glBindImageTexture(unit, texture, 0, GL_TRUE, 0, access, internalFormat);
                ASSERT_EQ(FirstGLError(), 0u)
                    << "glBindImageTexture refused layered format " << std::hex << internalFormat;
            }

            // Sets `name` from `values`, which must hold 4 * count floats.
            void SetVec4Array(GLuint program, const char* name, const std::vector<float>& values,
                              int count) {
                glUseProgram(program);
                const GLint location = glGetUniformLocation(program, name);
                ASSERT_GE(location, 0) << "the uniform array '" << name << "' was not reflected";
                glUniform4fv(location, count, values.data());
                EXPECT_EQ(FirstGLError(), 0u) << "setting '" << name << "' errored";
                glUseProgram(0);
            }

            std::vector<float> ReadFloatsFrom(GLenum target, GLuint texture, GLenum format,
                                              int componentsPerTexel, int texelCount) {
                std::vector<float> texels(static_cast<std::size_t>(texelCount) * componentsPerTexel,
                                          -12345.0f);
                glBindTexture(target, texture);
                glGetTexImage(target, 0, format, GL_FLOAT, texels.data());
                if (const GLenum error = FirstGLError()) {
                    ADD_FAILURE() << "reading the image back errored with " << GLErrorName(error);
                }
                return texels;
            }

            std::vector<GLuint> ReadUints(GLuint texture, GLenum format, int componentsPerTexel) {
                std::vector<GLuint> texels(static_cast<std::size_t>(kExtent) * kExtent * componentsPerTexel,
                                           0xFFFFFFFFu);
                glBindTexture(GL_TEXTURE_2D, texture);
                glGetTexImage(GL_TEXTURE_2D, 0, format, GL_UNSIGNED_INT, texels.data());
                if (const GLenum error = FirstGLError()) {
                    ADD_FAILURE() << "reading the image back errored with " << GLErrorName(error);
                }
                return texels;
            }
        };

        // GL_RG32F, the format all four allFormats walkers abort on (it is entry 2 of the
        // thirty-nine they step through, and none of them ever reached entry 3 on this backend).
        //
        // Two channels are written and two are not, and the shader asks for four back: what the
        // dispatch stores in b and a has to be dropped, and what the load returns for them has to
        // be GL's 0 and 1, not whatever the storage happens to hold. A widening that forgot the
        // store mask hands back (1, 2, 3, 4); one that widened the storage but not the bind reads
        // out of bounds and hands back anything at all; one that did not widen at all leaves the
        // seed, because the program never compiled.
        TEST_F(NonCoreImageFormatScenario, TwoChannelFloatImageDropsSurplusStoresAndLoadsZeroOne) {
            if (!Ready()) GTEST_SKIP() << "no GL context";
            if (!ImagesAreUsable()) GTEST_SKIP() << "no image load/store on this driver";

            const std::vector<float> seed(static_cast<std::size_t>(kExtent) * kExtent * 2u, -1.0f);
            const std::vector<float> wideSeed(static_cast<std::size_t>(kExtent) * kExtent * 4u, -1.0f);
            const GLuint narrow = MakeTexture(GL_RG32F, GL_RG, GL_FLOAT, seed.data());
            const GLuint wide = MakeTexture(GL_RGBA32F, GL_RGBA, GL_FLOAT, wideSeed.data());
            if (narrow == 0 || wide == 0) return;

            const GLuint storeProgram = MakeComputeProgram(R"(#version 430 core

layout (local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout (rg32f, binding = 0) writeonly uniform image2D narrow;

void main()
{
    imageStore(narrow, ivec2(gl_GlobalInvocationID.xy), vec4(1.0, 2.0, 3.0, 4.0));
}
)");
            // A SEPARATE program and a separate dispatch, so the load is ordered after the store
            // by glMemoryBarrier rather than by an in-shader barrier whose scope drivers disagree
            // about. It also means the loading program is built with its own image bindings, which
            // is the shape a rebuild bug would show up in.
            const GLuint loadProgram = MakeComputeProgram(R"(#version 430 core

layout (local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout (rg32f, binding = 0) readonly uniform image2D narrow;
layout (rgba32f, binding = 1) writeonly uniform image2D wide;

void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    imageStore(wide, coord, imageLoad(narrow, coord));
}
)");
            if (storeProgram == 0 || loadProgram == 0) return;

            BindImage(kNarrowUnit, narrow, GL_RG32F, GL_WRITE_ONLY);
            Dispatch(storeProgram);

            // The store reached the texture at all, read through the channels it really has.
            const std::vector<float> narrowTexels = ReadFloats(narrow, GL_RG, 2);
            for (int texel = 0; texel < kExtent * kExtent; ++texel) {
                EXPECT_FLOAT_EQ(narrowTexels[texel * 2 + 0], 1.0f) << "texel " << texel << " red";
                EXPECT_FLOAT_EQ(narrowTexels[texel * 2 + 1], 2.0f) << "texel " << texel << " green";
            }

            BindImage(kNarrowUnit, narrow, GL_RG32F, GL_READ_ONLY);
            BindImage(kWideUnit, wide, GL_RGBA32F, GL_WRITE_ONLY);
            Dispatch(loadProgram);

            const std::vector<float> loaded = ReadFloats(wide, GL_RGBA, 4);
            for (int texel = 0; texel < kExtent * kExtent; ++texel) {
                EXPECT_FLOAT_EQ(loaded[texel * 4 + 0], 1.0f) << "texel " << texel << " red";
                EXPECT_FLOAT_EQ(loaded[texel * 4 + 1], 2.0f) << "texel " << texel << " green";
                EXPECT_FLOAT_EQ(loaded[texel * 4 + 2], 0.0f)
                    << "texel " << texel << ": imageLoad on a two-channel format must report 0 for blue";
                EXPECT_FLOAT_EQ(loaded[texel * 4 + 3], 1.0f)
                    << "texel " << texel << ": imageLoad on a format without alpha must report 1";
            }
        }

        // GL_R11F_G11F_B10F, the format the allFormats and allTargets walkers stop at once the
        // channel widening has carried everything before it - and the one carrier that is NOT a
        // channel widening. It has no core format of its own per-channel width, so it is carried
        // in GL_RGBA16F, whose 5-bit exponent and longer mantissa hold every 11f (e5m6) and 10f
        // (e5m5) value exactly.
        //
        // What makes this case different from every other one here, and why it is worth its own
        // test: the frontend's shadow for this format is ONE PACKED 32-BIT WORD per texel, not
        // three components of the carrier's type. The upload therefore has to DECODE it, where
        // every other widening only pads channels onto data already in the right component type.
        // A widening that reused the channel repack reads three floats out of a four-byte texel
        // and shears the whole level - which a STORE test cannot see, because the dispatch
        // overwrites every texel the upload got wrong. So the seed here is per-texel distinct and
        // is checked through an imageLoad BEFORE anything is stored.
        //
        // Every constant is chosen to be exact in both encodings, so the comparisons can be
        // equality rather than tolerance: the 1/8 steps need three mantissa bits of the 11f
        // channels' six, and the 1/16 steps at exponent 1 need one of the 10f channel's five.
        TEST_F(NonCoreImageFormatScenario, PackedFloatImageDecodesItsUploadAndDropsSurplusStores) {
            if (!Ready()) GTEST_SKIP() << "no GL context";
            if (!ImagesAreUsable()) GTEST_SKIP() << "no image load/store on this driver";

            constexpr int kTexels = kExtent * kExtent;
            std::vector<float> seed(static_cast<std::size_t>(kTexels) * 3u, 0.0f);
            for (int texel = 0; texel < kTexels; ++texel) {
                seed[texel * 3 + 0] = 1.0f + static_cast<float>(texel) / 8.0f;
                seed[texel * 3 + 1] = 2.0f + static_cast<float>(texel) / 8.0f;
                seed[texel * 3 + 2] = 3.0f + static_cast<float>(texel) / 16.0f;
            }
            const std::vector<float> wideSeed(static_cast<std::size_t>(kTexels) * 4u, -1.0f);
            const GLuint narrow = MakeTexture(GL_R11F_G11F_B10F, GL_RGB, GL_FLOAT, seed.data());
            const GLuint wide = MakeTexture(GL_RGBA32F, GL_RGBA, GL_FLOAT, wideSeed.data());
            if (narrow == 0 || wide == 0) return;

            const GLuint loadProgram = MakeComputeProgram(R"(#version 430 core

layout (local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout (r11f_g11f_b10f, binding = 0) readonly uniform image2D narrow;
layout (rgba32f, binding = 1) writeonly uniform image2D wide;

void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    imageStore(wide, coord, imageLoad(narrow, coord));
}
)");
            const GLuint storeProgram = MakeComputeProgram(R"(#version 430 core

layout (local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout (r11f_g11f_b10f, binding = 0) writeonly uniform image2D narrow;

void main()
{
    imageStore(narrow, ivec2(gl_GlobalInvocationID.xy), vec4(5.0, 6.0, 7.0, 8.0));
}
)");
            if (loadProgram == 0 || storeProgram == 0) return;

            // THE UPLOAD, read back through the image. A sheared decode still produces plausible
            // floats, so the check is per texel and the seed never repeats a value.
            BindImage(kNarrowUnit, narrow, GL_R11F_G11F_B10F, GL_READ_ONLY);
            BindImage(kWideUnit, wide, GL_RGBA32F, GL_WRITE_ONLY);
            Dispatch(loadProgram);

            const std::vector<float> loaded = ReadFloats(wide, GL_RGBA, 4);
            for (int texel = 0; texel < kTexels; ++texel) {
                EXPECT_FLOAT_EQ(loaded[texel * 4 + 0], seed[texel * 3 + 0]) << "texel " << texel << " red";
                EXPECT_FLOAT_EQ(loaded[texel * 4 + 1], seed[texel * 3 + 1]) << "texel " << texel << " green";
                EXPECT_FLOAT_EQ(loaded[texel * 4 + 2], seed[texel * 3 + 2]) << "texel " << texel << " blue";
                EXPECT_FLOAT_EQ(loaded[texel * 4 + 3], 1.0f)
                    << "texel " << texel << ": imageLoad on a format without alpha must report 1";
            }

            // THE STORE. Three channels survive and the fourth is dropped, which is the mask this
            // format needs and no other widened format does - every other carrier here pins two
            // or three of the carrier's channels, this one pins only alpha.
            BindImage(kNarrowUnit, narrow, GL_R11F_G11F_B10F, GL_WRITE_ONLY);
            Dispatch(storeProgram);

            const std::vector<float> stored = ReadFloats(narrow, GL_RGB, 3);
            for (int texel = 0; texel < kTexels; ++texel) {
                EXPECT_FLOAT_EQ(stored[texel * 3 + 0], 5.0f) << "texel " << texel << " red";
                EXPECT_FLOAT_EQ(stored[texel * 3 + 1], 6.0f) << "texel " << texel << " green";
                EXPECT_FLOAT_EQ(stored[texel * 3 + 2], 7.0f) << "texel " << texel << " blue";
            }
        }

        // GL_RGB10_A2UI, the format all four allFormats walkers stop at once r11f_g11f_b10f is
        // carried - and the only widening whose carrier has as MANY channels as the original, so
        // GL leaves nothing to pin and neither access is rewritten. What it does need is the other
        // packed transfer: its shadow is one GL_UNSIGNED_INT_2_10_10_10_REV word per texel, which
        // the GL_RGBA16UI carrier is uploaded as four shorts.
        //
        // The seed is checked through an imageLoad BEFORE anything is stored, for the reason the
        // r11f case is: a sheared split still produces plausible integers, and a store would
        // overwrite every texel the upload got wrong. Every channel of every texel is distinct,
        // and the alpha values walk the whole 0..3 a two-bit channel has - a widening that pinned
        // alpha to GL's "1" the way a three-channel one must would pass for texel 1 alone.
        TEST_F(NonCoreImageFormatScenario, PackedIntegerImageSplitsItsUploadAndKeepsAllFourChannels) {
            if (!Ready()) GTEST_SKIP() << "no GL context";
            if (!ImagesAreUsable()) GTEST_SKIP() << "no image load/store on this driver";

            constexpr int kTexels = kExtent * kExtent;
            std::vector<GLuint> seed(static_cast<std::size_t>(kTexels), 0u);
            std::vector<GLuint> expected(static_cast<std::size_t>(kTexels) * 4u, 0u);
            for (int texel = 0; texel < kTexels; ++texel) {
                const GLuint r = static_cast<GLuint>(texel) * 7u;          // 0 .. 105
                const GLuint g = 1023u - static_cast<GLuint>(texel) * 11u; // 1023 .. 858
                const GLuint b = 512u + static_cast<GLuint>(texel);        // 512 .. 527
                const GLuint a = static_cast<GLuint>(texel) % 4u;          // the whole 0..3
                seed[texel] = r | (g << 10) | (b << 20) | (a << 30);
                expected[texel * 4 + 0] = r;
                expected[texel * 4 + 1] = g;
                expected[texel * 4 + 2] = b;
                expected[texel * 4 + 3] = a;
            }
            const std::vector<GLuint> wideSeed(static_cast<std::size_t>(kTexels) * 4u, 999u);
            const GLuint narrow =
                MakeTexture(GL_RGB10_A2UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT_2_10_10_10_REV, seed.data());
            const GLuint wide = MakeTexture(GL_RGBA32UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT, wideSeed.data());
            if (narrow == 0 || wide == 0) return;

            const GLuint loadProgram = MakeComputeProgram(R"(#version 430 core

layout (local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout (rgb10_a2ui, binding = 0) readonly uniform uimage2D narrow;
layout (rgba32ui, binding = 1) writeonly uniform uimage2D wide;

void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    imageStore(wide, coord, imageLoad(narrow, coord));
}
)");
            const GLuint storeProgram = MakeComputeProgram(R"(#version 430 core

layout (local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout (rgb10_a2ui, binding = 0) writeonly uniform uimage2D narrow;

void main()
{
    imageStore(narrow, ivec2(gl_GlobalInvocationID.xy), uvec4(11u, 22u, 33u, 2u));
}
)");
            if (loadProgram == 0 || storeProgram == 0) return;

            BindImage(kNarrowUnit, narrow, GL_RGB10_A2UI, GL_READ_ONLY);
            BindImage(kWideUnit, wide, GL_RGBA32UI, GL_WRITE_ONLY);
            Dispatch(loadProgram);

            const std::vector<GLuint> loaded = ReadUints(wide, GL_RGBA_INTEGER, 4);
            for (int texel = 0; texel < kTexels; ++texel) {
                EXPECT_EQ(loaded[texel * 4 + 0], expected[texel * 4 + 0]) << "texel " << texel << " red";
                EXPECT_EQ(loaded[texel * 4 + 1], expected[texel * 4 + 1]) << "texel " << texel << " green";
                EXPECT_EQ(loaded[texel * 4 + 2], expected[texel * 4 + 2]) << "texel " << texel << " blue";
                EXPECT_EQ(loaded[texel * 4 + 3], expected[texel * 4 + 3]) << "texel " << texel << " alpha";
            }

            // THE STORE. All four channels survive - this is the one widened format where GL drops
            // nothing, so a mask here would be a bug rather than the emulation.
            BindImage(kNarrowUnit, narrow, GL_RGB10_A2UI, GL_WRITE_ONLY);
            Dispatch(storeProgram);

            const std::vector<GLuint> stored = ReadUints(narrow, GL_RGBA_INTEGER, 4);
            for (int texel = 0; texel < kTexels; ++texel) {
                EXPECT_EQ(stored[texel * 4 + 0], 11u) << "texel " << texel << " red";
                EXPECT_EQ(stored[texel * 4 + 1], 22u) << "texel " << texel << " green";
                EXPECT_EQ(stored[texel * 4 + 2], 33u) << "texel " << texel << " blue";
                EXPECT_EQ(stored[texel * 4 + 3], 2u) << "texel " << texel << " alpha";
            }
        }

        // GL_R8UI: the only format KHR-GL43.shader_image_load_store.single-byte_data_alignment
        // declares, and one SPIRV-Cross refuses to print for ESSL at all, so before the emulation
        // no text was produced for the stage and the dispatch could not run.
        //
        // THREE added channels rather than one, and an INTEGER 1 rather than a saturated field -
        // GL_UNSIGNED_BYTE serves both GL_R8 and GL_R8UI, so a widening that decided the missing
        // alpha from the transfer type instead of from the format hands back 255 here.
        TEST_F(NonCoreImageFormatScenario, SingleChannelUnsignedImageDropsSurplusStoresAndLoadsZeroOne) {
            if (!Ready()) GTEST_SKIP() << "no GL context";
            if (!ImagesAreUsable()) GTEST_SKIP() << "no image load/store on this driver";

            const std::vector<GLubyte> seed(static_cast<std::size_t>(kExtent) * kExtent, 200u);
            const std::vector<GLuint> wideSeed(static_cast<std::size_t>(kExtent) * kExtent * 4u, 999u);
            const GLuint narrow = MakeTexture(GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, seed.data());
            const GLuint wide = MakeTexture(GL_RGBA32UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT, wideSeed.data());
            if (narrow == 0 || wide == 0) return;

            const GLuint storeProgram = MakeComputeProgram(R"(#version 430 core

layout (local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout (r8ui, binding = 0) writeonly uniform uimage2D narrow;

void main()
{
    imageStore(narrow, ivec2(gl_GlobalInvocationID.xy), uvec4(7u, 8u, 9u, 10u));
}
)");
            const GLuint loadProgram = MakeComputeProgram(R"(#version 430 core

layout (local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout (r8ui, binding = 0) readonly uniform uimage2D narrow;
layout (rgba32ui, binding = 1) writeonly uniform uimage2D wide;

void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    imageStore(wide, coord, imageLoad(narrow, coord));
}
)");
            if (storeProgram == 0 || loadProgram == 0) return;

            BindImage(kNarrowUnit, narrow, GL_R8UI, GL_WRITE_ONLY);
            Dispatch(storeProgram);

            const std::vector<GLuint> narrowTexels = ReadUints(narrow, GL_RED_INTEGER, 1);
            for (int texel = 0; texel < kExtent * kExtent; ++texel) {
                EXPECT_EQ(narrowTexels[texel], 7u) << "texel " << texel << " red";
            }

            BindImage(kNarrowUnit, narrow, GL_R8UI, GL_READ_ONLY);
            BindImage(kWideUnit, wide, GL_RGBA32UI, GL_WRITE_ONLY);
            Dispatch(loadProgram);

            const std::vector<GLuint> loaded = ReadUints(wide, GL_RGBA_INTEGER, 4);
            for (int texel = 0; texel < kExtent * kExtent; ++texel) {
                EXPECT_EQ(loaded[texel * 4 + 0], 7u) << "texel " << texel << " red";
                EXPECT_EQ(loaded[texel * 4 + 1], 0u)
                    << "texel " << texel << ": imageLoad on a one-channel format must report 0 for green";
                EXPECT_EQ(loaded[texel * 4 + 2], 0u)
                    << "texel " << texel << ": imageLoad on a one-channel format must report 0 for blue";
                EXPECT_EQ(loaded[texel * 4 + 3], 1u)
                    << "texel " << texel
                    << ": imageLoad on an INTEGER format without alpha must report the integer 1";
            }
        }

        // GL_RG16, the first of the seven NORMALIZED formats and the first carrier that changes the
        // shader-visible TYPE: core ESSL has no 16-bit normalized image format of any width, and
        // no float carrier is honest either (a half has eleven mantissa bits against sixteen), so
        // the rgba16ui behind it holds the format's own CODES and every access converts.
        //
        // Both directions of GL 4.6 2.3.5 are checked, and the STORE direction is checked as exact
        // INTEGER CODES rather than as floats within a tolerance - which is the point of a code
        // carrier over a float one, and the only thing that would catch a rounding rule that was
        // merely close. The values are chosen so that the products are exact in float32: 0.25 and
        // 0.75 land off a tie, 0.5 lands exactly ON one (0.5 * 65535 = 32767.5), and the two
        // out-of-range values must be clamped before they are rounded rather than after.
        TEST_F(NonCoreImageFormatScenario, UnsignedNormalizedImageCarriesItsCodesBothWays) {
            if (!Ready()) GTEST_SKIP() << "no GL context";
            if (!ImagesAreUsable()) GTEST_SKIP() << "no image load/store on this driver";

            constexpr int kTexels = kExtent * kExtent;
            constexpr double kUnorm16Max = 65535.0;

            // THE UPLOAD. Distinct per texel, and the codes are the shadow's own 16-bit words: a
            // widening that padded or sheared them still produces plausible normalized floats.
            std::vector<GLushort> seed(static_cast<std::size_t>(kTexels) * 2u, 0);
            for (int texel = 0; texel < kTexels; ++texel) {
                seed[texel * 2 + 0] = static_cast<GLushort>(texel * 4001);
                seed[texel * 2 + 1] = static_cast<GLushort>(65535 - texel * 3001);
            }
            const std::vector<float> wideSeed(static_cast<std::size_t>(kTexels) * 4u, -1.0f);
            const GLuint narrow = MakeTexture(GL_RG16, GL_RG, GL_UNSIGNED_SHORT, seed.data());
            const GLuint wide = MakeTexture(GL_RGBA32F, GL_RGBA, GL_FLOAT, wideSeed.data());
            if (narrow == 0 || wide == 0) return;

            const GLuint loadProgram = MakeComputeProgram(R"(#version 430 core

layout (local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout (rg16, binding = 0) readonly uniform image2D narrow;
layout (rgba32f, binding = 1) writeonly uniform image2D wide;

void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    imageStore(wide, coord, imageLoad(narrow, coord));
}
)");
            const GLuint storeProgram = MakeComputeProgram(R"(#version 430 core

layout (local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout (rg16, binding = 0) writeonly uniform image2D narrow;
uniform vec4 g_values[16];

void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    imageStore(narrow, coord, g_values[coord.y * 4 + coord.x]);
}
)");
            if (loadProgram == 0 || storeProgram == 0) return;

            BindImage(kNarrowUnit, narrow, GL_RG16, GL_READ_ONLY);
            BindImage(kWideUnit, wide, GL_RGBA32F, GL_WRITE_ONLY);
            Dispatch(loadProgram);

            std::vector<float> loaded = ReadFloats(wide, GL_RGBA, 4);
            for (int texel = 0; texel < kTexels; ++texel) {
                EXPECT_EQ(std::lround(loaded[texel * 4 + 0] * kUnorm16Max), seed[texel * 2 + 0])
                    << "texel " << texel << " red";
                EXPECT_EQ(std::lround(loaded[texel * 4 + 1] * kUnorm16Max), seed[texel * 2 + 1])
                    << "texel " << texel << " green";
                EXPECT_FLOAT_EQ(loaded[texel * 4 + 2], 0.0f)
                    << "texel " << texel << ": imageLoad on a two-channel format must report 0 for blue";
                EXPECT_FLOAT_EQ(loaded[texel * 4 + 3], 1.0f)
                    << "texel " << texel << ": imageLoad on a format without alpha must report 1";
            }

            // THE STORE, per GL 4.6 2.3.5: c = round(clamp(f, 0, 1) * (2^b - 1)), with a tie
            // rounded away from zero.
            struct Boundary {
                float value;
                long code;
            };
            const Boundary boundaries[kTexels] = {
                {0.0f, 0},          {1.0f, 65535},      {0.5f, 32768},      {0.25f, 16384},
                {0.75f, 49151},     {-0.5f, 0},         {2.0f, 65535},      {-1.0f, 0},
                {1.0f / 131072.0f, 0},                  // 0.4999923 of a code: rounds DOWN
                {3.0f / 131072.0f, 1},                  // 1.4999771 of a code: rounds DOWN to 1
                {1.0f / 65535.0f, 1},                   // exactly one code
                {32767.0f / 65535.0f, 32767},           {32768.0f / 65535.0f, 32768},
                // 0.125 * 65535 = 8191.875 and 0.875 * 65535 = 57343.125 - neither is a tie, and
                // both round DOWN, which is the pair that catches a conversion that scaled by 2^b
                // instead of 2^b - 1.
                {65534.0f / 65535.0f, 65534},           {0.125f, 8192},     {0.875f, 57343},
            };
            std::vector<float> values(static_cast<std::size_t>(kTexels) * 4u, 0.0f);
            for (int texel = 0; texel < kTexels; ++texel) {
                values[texel * 4 + 0] = boundaries[texel].value;
                values[texel * 4 + 1] = boundaries[texel].value;
                values[texel * 4 + 2] = 0.5f; // dropped: a two-channel format has no blue
                values[texel * 4 + 3] = 0.5f; // dropped: nor an alpha
            }
            SetVec4Array(storeProgram, "g_values", values, kTexels);

            BindImage(kNarrowUnit, narrow, GL_RG16, GL_WRITE_ONLY);
            Dispatch(storeProgram);

            // Read back through the IMAGE, so what is compared is the code the store actually
            // wrote rather than anything the readback path might renormalize on its own.
            BindImage(kNarrowUnit, narrow, GL_RG16, GL_READ_ONLY);
            BindImage(kWideUnit, wide, GL_RGBA32F, GL_WRITE_ONLY);
            Dispatch(loadProgram);

            loaded = ReadFloats(wide, GL_RGBA, 4);
            for (int texel = 0; texel < kTexels; ++texel) {
                EXPECT_EQ(std::lround(loaded[texel * 4 + 0] * kUnorm16Max), boundaries[texel].code)
                    << "texel " << texel << " stored " << boundaries[texel].value;
                EXPECT_EQ(std::lround(loaded[texel * 4 + 1] * kUnorm16Max), boundaries[texel].code)
                    << "texel " << texel << " green";
                EXPECT_FLOAT_EQ(loaded[texel * 4 + 2], 0.0f) << "texel " << texel << " blue";
                EXPECT_FLOAT_EQ(loaded[texel * 4 + 3], 1.0f) << "texel " << texel << " alpha";
            }

            // ...and glGetTexImage owes the application the NORMALIZED value, whatever the ES
            // storage holds. The whole texture is an integer one now, so this is the only place
            // the readback conversion is exercised at all.
            const std::vector<float> viaGetTexImage = ReadFloats(narrow, GL_RG, 2);
            for (int texel = 0; texel < kTexels; ++texel) {
                EXPECT_EQ(std::lround(viaGetTexImage[texel * 2 + 0] * kUnorm16Max), boundaries[texel].code)
                    << "texel " << texel << " red through glGetTexImage";
                EXPECT_EQ(std::lround(viaGetTexImage[texel * 2 + 1] * kUnorm16Max), boundaries[texel].code)
                    << "texel " << texel << " green through glGetTexImage";
            }
        }

        // GL_RGBA16_SNORM, the signed twin. Two things differ and both are one-line mistakes: the
        // code is a two's-complement 16-bit integer stored in an UNSIGNED carrier channel, so it
        // has to be sign-extended on the way out (a zero extension reads every negative value as
        // something near +1), and the decode is max(c / 32767, -1) rather than the bare division,
        // because the code -32768 exists and GL says it means exactly -1.
        TEST_F(NonCoreImageFormatScenario, SignedNormalizedImageSignExtendsItsCodesAndClampsAtMinusOne) {
            if (!Ready()) GTEST_SKIP() << "no GL context";
            if (!ImagesAreUsable()) GTEST_SKIP() << "no image load/store on this driver";

            constexpr int kTexels = kExtent * kExtent;
            constexpr double kSnorm16Max = 32767.0;

            // The seed walks the whole signed range, INCLUDING -32768, whose decode is the one
            // value the division alone gets wrong.
            const GLshort seedCodes[kTexels] = {0,     32767, -32767, -32768, 1,     -1,    16384, -16384,
                                                12345, -12345, 32766, -32766, 255,   -256,  4095,  -4096};
            std::vector<GLshort> seed(static_cast<std::size_t>(kTexels) * 4u, 0);
            for (int texel = 0; texel < kTexels; ++texel) {
                seed[texel * 4 + 0] = seedCodes[texel];
                seed[texel * 4 + 1] = static_cast<GLshort>(-seedCodes[texel] == -32768 ? 32767
                                                                                       : -seedCodes[texel]);
                seed[texel * 4 + 2] = seedCodes[(texel + 1) % kTexels];
                seed[texel * 4 + 3] = seedCodes[(texel + 2) % kTexels];
            }
            const std::vector<float> wideSeed(static_cast<std::size_t>(kTexels) * 4u, -12.0f);
            const GLuint narrow = MakeTexture(GL_RGBA16_SNORM, GL_RGBA, GL_SHORT, seed.data());
            const GLuint wide = MakeTexture(GL_RGBA32F, GL_RGBA, GL_FLOAT, wideSeed.data());
            if (narrow == 0 || wide == 0) return;

            const GLuint loadProgram = MakeComputeProgram(R"(#version 430 core

layout (local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout (rgba16_snorm, binding = 0) readonly uniform image2D narrow;
layout (rgba32f, binding = 1) writeonly uniform image2D wide;

void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    imageStore(wide, coord, imageLoad(narrow, coord));
}
)");
            const GLuint storeProgram = MakeComputeProgram(R"(#version 430 core

layout (local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout (rgba16_snorm, binding = 0) writeonly uniform image2D narrow;
uniform vec4 g_values[16];

void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    imageStore(narrow, coord, g_values[coord.y * 4 + coord.x]);
}
)");
            if (loadProgram == 0 || storeProgram == 0) return;

            BindImage(kNarrowUnit, narrow, GL_RGBA16_SNORM, GL_READ_ONLY);
            BindImage(kWideUnit, wide, GL_RGBA32F, GL_WRITE_ONLY);
            Dispatch(loadProgram);

            std::vector<float> loaded = ReadFloats(wide, GL_RGBA, 4);
            for (int texel = 0; texel < kTexels; ++texel) {
                for (int channel = 0; channel < 4; ++channel) {
                    const GLshort code = seed[texel * 4 + channel];
                    const float expected =
                        std::max(static_cast<float>(code) / static_cast<float>(kSnorm16Max), -1.0f);
                    EXPECT_FLOAT_EQ(loaded[texel * 4 + channel], expected)
                        << "texel " << texel << " channel " << channel << " code " << code;
                }
            }

            // THE STORE: c = round(clamp(f, -1, 1) * (2^(b-1) - 1)), ties away from zero on BOTH
            // sides - which is what makes -0.5 land on -16384 rather than on -16383.
            struct Boundary {
                float value;
                long code;
            };
            const Boundary boundaries[kTexels] = {
                {0.0f, 0},        {1.0f, 32767},    {-1.0f, -32767},  {0.5f, 16384},
                {-0.5f, -16384},  {2.0f, 32767},    {-2.0f, -32767},  {0.25f, 8192},
                {-0.25f, -8192},  {1.0f / 32767.0f, 1},               {-1.0f / 32767.0f, -1},
                // Three quarters of a code, not half: GL leaves the direction of a TIE to the
                // implementation ("if two values are equally near, the implementation may choose
                // either"), and Magma hands these formats to Vulkan unemulated, so a value exactly
                // on 0.5 of a code is the one thing the two backends are allowed to disagree
                // about. Every entry here is off a tie except the ones at 0.5 and 0.25 of the
                // RANGE, whose products (16383.5 and 8192) round the same way under either rule.
                {0.75f / 32767.0f, 1},              {-0.75f / 32767.0f, -1},
                {16383.0f / 32767.0f, 16383},       {-16383.0f / 32767.0f, -16383},
                {0.125f, 4096},
            };
            std::vector<float> values(static_cast<std::size_t>(kTexels) * 4u, 0.0f);
            for (int texel = 0; texel < kTexels; ++texel) {
                for (int channel = 0; channel < 4; ++channel) {
                    values[texel * 4 + channel] = boundaries[texel].value;
                }
            }
            SetVec4Array(storeProgram, "g_values", values, kTexels);

            BindImage(kNarrowUnit, narrow, GL_RGBA16_SNORM, GL_WRITE_ONLY);
            Dispatch(storeProgram);

            BindImage(kNarrowUnit, narrow, GL_RGBA16_SNORM, GL_READ_ONLY);
            BindImage(kWideUnit, wide, GL_RGBA32F, GL_WRITE_ONLY);
            Dispatch(loadProgram);

            loaded = ReadFloats(wide, GL_RGBA, 4);
            for (int texel = 0; texel < kTexels; ++texel) {
                for (int channel = 0; channel < 4; ++channel) {
                    EXPECT_EQ(std::lround(loaded[texel * 4 + channel] * kSnorm16Max),
                              boundaries[texel].code)
                        << "texel " << texel << " channel " << channel << " stored "
                        << boundaries[texel].value;
                }
            }
        }

        // GL_RGB10_A2, the one normalized format whose channels are not all the same width: three
        // of ten bits and one of two. A single denominator would be right for three quarters of
        // every texel and wildly wrong for the fourth - alpha 1.0 would come back as 3/1023.
        TEST_F(NonCoreImageFormatScenario, TenTenTenTwoImageUsesItsOwnPerChannelDenominators) {
            if (!Ready()) GTEST_SKIP() << "no GL context";
            if (!ImagesAreUsable()) GTEST_SKIP() << "no image load/store on this driver";

            constexpr int kTexels = kExtent * kExtent;

            std::vector<GLuint> seed(static_cast<std::size_t>(kTexels), 0u);
            std::vector<GLuint> seedCodes(static_cast<std::size_t>(kTexels) * 4u, 0u);
            for (int texel = 0; texel < kTexels; ++texel) {
                const GLuint r = static_cast<GLuint>(texel) * 67u;
                const GLuint g = 1023u - static_cast<GLuint>(texel) * 13u;
                const GLuint b = 341u + static_cast<GLuint>(texel);
                const GLuint a = static_cast<GLuint>(texel) % 4u;
                seed[texel] = r | (g << 10) | (b << 20) | (a << 30);
                seedCodes[texel * 4 + 0] = r;
                seedCodes[texel * 4 + 1] = g;
                seedCodes[texel * 4 + 2] = b;
                seedCodes[texel * 4 + 3] = a;
            }
            const std::vector<float> wideSeed(static_cast<std::size_t>(kTexels) * 4u, -1.0f);
            const GLuint narrow =
                MakeTexture(GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, seed.data());
            const GLuint wide = MakeTexture(GL_RGBA32F, GL_RGBA, GL_FLOAT, wideSeed.data());
            if (narrow == 0 || wide == 0) return;

            const GLuint loadProgram = MakeComputeProgram(R"(#version 430 core

layout (local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout (rgb10_a2, binding = 0) readonly uniform image2D narrow;
layout (rgba32f, binding = 1) writeonly uniform image2D wide;

void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    imageStore(wide, coord, imageLoad(narrow, coord));
}
)");
            const GLuint storeProgram = MakeComputeProgram(R"(#version 430 core

layout (local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout (rgb10_a2, binding = 0) writeonly uniform image2D narrow;

void main()
{
    imageStore(narrow, ivec2(gl_GlobalInvocationID.xy), vec4(0.0, 0.5, 1.0, 1.0));
}
)");
            if (loadProgram == 0 || storeProgram == 0) return;

            BindImage(kNarrowUnit, narrow, GL_RGB10_A2, GL_READ_ONLY);
            BindImage(kWideUnit, wide, GL_RGBA32F, GL_WRITE_ONLY);
            Dispatch(loadProgram);

            std::vector<float> loaded = ReadFloats(wide, GL_RGBA, 4);
            for (int texel = 0; texel < kTexels; ++texel) {
                for (int channel = 0; channel < 4; ++channel) {
                    const auto denominator = channel == 3 ? 3.0f : 1023.0f;
                    EXPECT_FLOAT_EQ(loaded[texel * 4 + channel],
                                    static_cast<float>(seedCodes[texel * 4 + channel]) / denominator)
                        << "texel " << texel << " channel " << channel;
                }
            }

            // 0.5 through a TWO-bit channel is 1.5 of a code and rounds away from zero to 2, which
            // is 2/3 back - a value only the two-bit denominator can produce.
            BindImage(kNarrowUnit, narrow, GL_RGB10_A2, GL_WRITE_ONLY);
            Dispatch(storeProgram);

            BindImage(kNarrowUnit, narrow, GL_RGB10_A2, GL_READ_ONLY);
            BindImage(kWideUnit, wide, GL_RGBA32F, GL_WRITE_ONLY);
            Dispatch(loadProgram);

            loaded = ReadFloats(wide, GL_RGBA, 4);
            for (int texel = 0; texel < kTexels; ++texel) {
                EXPECT_EQ(std::lround(loaded[texel * 4 + 0] * 1023.0), 0) << "texel " << texel << " red";
                EXPECT_EQ(std::lround(loaded[texel * 4 + 1] * 1023.0), 512) << "texel " << texel << " green";
                EXPECT_EQ(std::lround(loaded[texel * 4 + 2] * 1023.0), 1023) << "texel " << texel << " blue";
                EXPECT_EQ(std::lround(loaded[texel * 4 + 3] * 3.0), 3) << "texel " << texel << " alpha";
            }
        }

        // The same carrier on a GL_TEXTURE_CUBE_MAP_ARRAY, the target the allTargets walkers reach
        // last and the one whose ES equivalent is addressed differently from its GL name (six
        // layer-faces per cube, as array layers). Nothing about the format conversion changes with
        // the target - which is exactly the claim, since the storage widening, the layered bind and
        // the per-layer readback all have their own code paths for this target alone.
        TEST_F(NonCoreImageFormatScenario, NormalizedImageCarriesEveryLayerFaceOfACubeMapArray) {
            if (!Ready()) GTEST_SKIP() << "no GL context";
            if (!ImagesAreUsable()) GTEST_SKIP() << "no image load/store on this driver";
            GLint maxComputeImageUniforms = 0;
            glGetIntegerv(GL_MAX_COMPUTE_IMAGE_UNIFORMS, &maxComputeImageUniforms);
            while (glGetError() != GL_NO_ERROR) {
            }

            constexpr int kCubes = 2;
            constexpr int kLayerFaces = 6 * kCubes;
            constexpr int kTexelsPerFace = kExtent * kExtent;
            constexpr int kTexels = kTexelsPerFace * kLayerFaces;
            constexpr double kUnorm16Max = 65535.0;

            std::vector<GLushort> seed(static_cast<std::size_t>(kTexels), 0);
            for (int texel = 0; texel < kTexels; ++texel) {
                seed[texel] = static_cast<GLushort>((texel * 5477u) & 0xFFFFu);
            }
            const GLuint narrow =
                MakeCubeArrayTexture(GL_R16, GL_RED, GL_UNSIGNED_SHORT, seed.data(), kCubes);
            if (narrow == 0) return;

            // THE UPLOAD, read back through glGetTexImage across every layer-face. A carrier that
            // widened the storage but seeded only the first face leaves the rest at zero, which is
            // what the per-layer readback path is there to catch, and this target is the only one
            // whose readback goes layer by layer.
            const std::vector<float> uploaded =
                ReadFloatsFrom(GL_TEXTURE_CUBE_MAP_ARRAY, narrow, GL_RED, 1, kTexels);
            for (int texel = 0; texel < kTexels; ++texel) {
                EXPECT_EQ(std::lround(uploaded[texel] * kUnorm16Max), seed[texel])
                    << "texel " << texel << " of " << kTexels;
            }

            // ...and through an imageCubeArray, which is the declaration the shader half has to
            // carry for this target: a layered bind, an ivec3 coordinate whose z is the
            // layer-face, and the same unpack as every other target.
            const std::vector<float> wideSeed(static_cast<std::size_t>(kTexelsPerFace) * 4u, -1.0f);
            const GLuint wide = MakeTexture(GL_RGBA32F, GL_RGBA, GL_FLOAT, wideSeed.data());
            if (wide == 0) return;
            const GLuint loadProgram = MakeComputeProgram(R"(#version 430 core

layout (local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout (r16, binding = 0) readonly uniform imageCubeArray narrow;
layout (rgba32f, binding = 1) writeonly uniform image2D wide;
uniform int g_layerFace;

void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    imageStore(wide, coord, imageLoad(narrow, ivec3(coord, g_layerFace)));
}
)");
            if (loadProgram == 0) return;

            // Two faces, one of them past the first cube, so a carrier that addressed only the
            // first six layer-faces cannot pass.
            for (const int layerFace : {1, 9}) {
                glUseProgram(loadProgram);
                const GLint location = glGetUniformLocation(loadProgram, "g_layerFace");
                ASSERT_GE(location, 0) << "g_layerFace was not reflected";
                glUniform1i(location, layerFace);
                glUseProgram(0);

                BindLayeredImage(kNarrowUnit, narrow, GL_R16, GL_READ_ONLY);
                BindImage(kWideUnit, wide, GL_RGBA32F, GL_WRITE_ONLY);
                Dispatch(loadProgram);

                const std::vector<float> loaded = ReadFloats(wide, GL_RGBA, 4);
                for (int texel = 0; texel < kTexelsPerFace; ++texel) {
                    const int sourceTexel = layerFace * kTexelsPerFace + texel;
                    EXPECT_EQ(std::lround(loaded[texel * 4 + 0] * kUnorm16Max), seed[sourceTexel])
                        << "layer-face " << layerFace << " texel " << texel;
                    EXPECT_FLOAT_EQ(loaded[texel * 4 + 1], 0.0f)
                        << "layer-face " << layerFace << " texel " << texel
                        << ": imageLoad on a one-channel format must report 0 for green";
                    EXPECT_FLOAT_EQ(loaded[texel * 4 + 3], 1.0f)
                        << "layer-face " << layerFace << " texel " << texel
                        << ": imageLoad on a format without alpha must report 1";
                }
            }
        }

        // A BUFFER image, which takes neither of the emulations above. Its texels are the
        // application's buffer object - at the size and layout the application gave it, and
        // usually also a vertex, index or storage buffer - so there is nothing to reallocate a
        // carrier in. What CAN be done is a SPLIT: rg32f over N texels and r32f over 2N texels
        // describe exactly the same bytes, so the view is re-declared and every subscript is
        // doubled (WidenImageFormatsPass, and the matching glTexBuffer/glBindImageTexture format
        // in TextureImpl).
        //
        // THE NUMBERS HERE ARE THE ONES THAT PINNED THE OLD BUG. Widening a buffer image instead
        // leaves the shader striding 16 bytes through 8-byte texels: measured on an Adreno 830
        // with this exact 32-byte GL_RG32F buffer and this exact shader, the readback came back
        // [1,100] [0,1] [2,100] [0,1] - texels 0 and 1 landed on top of all four, and texels 2 and
        // 3 were written past the end of the application's buffer.
        //
        // This runs on every backend, and on a driver that CAN spell rg32f for an imageBuffer
        // (Mesa's, which the software lanes use) nothing is split at all - which is the other half
        // of the claim: the arming has to agree with the shader, so a split that fired where the
        // driver needed none would double every subscript and fail here just as loudly.
        TEST_F(NonCoreImageFormatScenario, BufferImageAddressesTheApplicationsOwnTexels) {
            if (!Ready()) GTEST_SKIP() << "no GL context";
            if (!ImagesAreUsable()) GTEST_SKIP() << "no image load/store on this driver";
            GLint maxTextureBufferSize = 0;
            glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &maxTextureBufferSize);
            while (glGetError() != GL_NO_ERROR) {
            }
            if (maxTextureBufferSize <= 0) GTEST_SKIP() << "no buffer textures on this driver";

            constexpr int kBufferTexels = 4;
            const std::vector<float> seed(static_cast<std::size_t>(kBufferTexels) * 2u, -1.0f);

            GLuint buffer = 0;
            glGenBuffers(1, &buffer);
            glBindBuffer(GL_TEXTURE_BUFFER, buffer);
            glBufferData(GL_TEXTURE_BUFFER, static_cast<GLsizeiptr>(seed.size() * sizeof(float)), seed.data(),
                         GL_DYNAMIC_DRAW);
            GLuint texture = 0;
            glGenTextures(1, &texture);
            m_textures.push_back(texture);
            glBindTexture(GL_TEXTURE_BUFFER, texture);
            glTexBuffer(GL_TEXTURE_BUFFER, GL_RG32F, buffer);
            if (const GLenum error = FirstGLError()) {
                glDeleteBuffers(1, &buffer);
                GTEST_SKIP() << "glTexBuffer(GL_RG32F) errored with " << GLErrorName(error);
            }

            const GLuint storeProgram = MakeComputeProgram(R"(#version 430 core

layout (local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout (rg32f, binding = 0) writeonly uniform imageBuffer narrow;

void main()
{
    int texel = int(gl_GlobalInvocationID.x);
    imageStore(narrow, texel, vec4(float(texel + 1), 100.0, 3.0, 4.0));
}
)");
            if (storeProgram == 0) {
                glDeleteBuffers(1, &buffer);
                return;
            }

            BindImage(kNarrowUnit, texture, GL_RG32F, GL_WRITE_ONLY);
            glUseProgram(storeProgram);
            glDispatchCompute(kBufferTexels, 1, 1);
            glMemoryBarrier(GL_ALL_BARRIER_BITS);
            EXPECT_EQ(FirstGLError(), 0u) << "the dispatch leaked a GL error";
            glUseProgram(0);

            std::vector<float> readback(seed.size(), -12345.0f);
            glBindBuffer(GL_TEXTURE_BUFFER, buffer);
            glGetBufferSubData(GL_TEXTURE_BUFFER, 0,
                               static_cast<GLsizeiptr>(readback.size() * sizeof(float)), readback.data());
            EXPECT_EQ(FirstGLError(), 0u) << "reading the buffer back errored";

            for (int texel = 0; texel < kBufferTexels; ++texel) {
                EXPECT_FLOAT_EQ(readback[texel * 2 + 0], static_cast<float>(texel + 1))
                    << "texel " << texel << " red";
                EXPECT_FLOAT_EQ(readback[texel * 2 + 1], 100.0f) << "texel " << texel << " green";
            }

            glBindBuffer(GL_TEXTURE_BUFFER, 0);
            glDeleteBuffers(1, &buffer);
            while (glGetError() != GL_NO_ERROR) {
            }
        }

        // The SAME buffer texture read through BOTH doors at once, which is the shape the split
        // originally broke. A buffer texture that is image-bound is split - the view is re-declared
        // one component at a time and every image subscript is doubled to match - but the sampler
        // side is NOT subscript-rewritten, so re-describing the APPLICATION's own texture made
        // texelFetch(s, i) return component 2i of the base view instead of texel i's whole pair.
        // The split therefore goes on a private second name over the same buffer
        // (BackendTextureObject::m_bufferImageSplitViewId) and the application's name keeps the
        // format it asked for: rg32f is a legal SAMPLED buffer-texture format in ES 3.2, it is only
        // the IMAGE binding ES cannot spell.
        //
        // This is KHR-GL42/43.shader_image_load_store.advanced-sync-imageAccess reduced to one
        // dispatch. That case image-stores into a GL_RG32F buffer texture and then, in one shader,
        // reads the same texture through an imageBuffer AND a samplerBuffer and compares the two -
        // so it went red on every pixel while its sibling -vertexArray, which never samples the
        // buffer texture, passed.
        //
        // Like the case above this runs on every backend, and on a driver that can spell rg32f for
        // an imageBuffer nothing is split at all - both doors then trivially agree, which is the
        // other half of the claim: a split that fired where the driver needed none would show up
        // here as the two disagreeing.
        TEST_F(NonCoreImageFormatScenario, ASplitBufferImageStillSamplesWholeTexels) {
            if (!Ready()) GTEST_SKIP() << "no GL context";
            if (!ImagesAreUsable()) GTEST_SKIP() << "no image load/store on this driver";
            GLint maxTextureBufferSize = 0;
            glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &maxTextureBufferSize);
            while (glGetError() != GL_NO_ERROR) {
            }
            if (maxTextureBufferSize <= 0) GTEST_SKIP() << "no buffer textures on this driver";

            constexpr int kBufferTexels = 4;
            // Both components of every texel distinct and non-zero, so a sampler that reads the
            // SPLIT view cannot accidentally agree: texel i would come back as (2i-th component,
            // 0, 0, 1) rather than (x, y, 0, 1), and every one of those is a value no texel holds.
            std::vector<float> seed(static_cast<std::size_t>(kBufferTexels) * 2u, 0.0f);
            for (int texel = 0; texel < kBufferTexels; ++texel) {
                seed[static_cast<std::size_t>(texel) * 2u + 0u] = static_cast<float>(texel * 10 + 1);
                seed[static_cast<std::size_t>(texel) * 2u + 1u] = static_cast<float>(texel * 10 + 2);
            }

            GLuint buffer = 0;
            glGenBuffers(1, &buffer);
            glBindBuffer(GL_TEXTURE_BUFFER, buffer);
            glBufferData(GL_TEXTURE_BUFFER, static_cast<GLsizeiptr>(seed.size() * sizeof(float)), seed.data(),
                         GL_DYNAMIC_DRAW);
            GLuint texture = 0;
            glGenTextures(1, &texture);
            m_textures.push_back(texture);
            glBindTexture(GL_TEXTURE_BUFFER, texture);
            glTexBuffer(GL_TEXTURE_BUFFER, GL_RG32F, buffer);
            if (const GLenum error = FirstGLError()) {
                glDeleteBuffers(1, &buffer);
                GTEST_SKIP() << "glTexBuffer(GL_RG32F) errored with " << GLErrorName(error);
            }

            // The answer buffer is rgba32f, which IS core ESSL, so it is never split and cannot
            // hide a mistake in the thing under test.
            constexpr int kAnswers = kBufferTexels * 2;
            const std::vector<float> answerSeed(static_cast<std::size_t>(kAnswers) * 4u, -12345.0f);
            GLuint answerBuffer = 0;
            glGenBuffers(1, &answerBuffer);
            glBindBuffer(GL_TEXTURE_BUFFER, answerBuffer);
            glBufferData(GL_TEXTURE_BUFFER, static_cast<GLsizeiptr>(answerSeed.size() * sizeof(float)),
                         answerSeed.data(), GL_DYNAMIC_DRAW);
            GLuint answerTexture = 0;
            glGenTextures(1, &answerTexture);
            m_textures.push_back(answerTexture);
            glBindTexture(GL_TEXTURE_BUFFER, answerTexture);
            glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, answerBuffer);
            if (const GLenum error = FirstGLError()) {
                glDeleteBuffers(1, &buffer);
                glDeleteBuffers(1, &answerBuffer);
                GTEST_SKIP() << "glTexBuffer(GL_RGBA32F) errored with " << GLErrorName(error);
            }

            const GLuint program = MakeComputeProgram(R"(#version 430 core

layout (local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout (rg32f, binding = 0) readonly uniform imageBuffer narrow;
layout (rgba32f, binding = 1) writeonly uniform imageBuffer answers;
uniform samplerBuffer sampled;

void main()
{
    int texel = int(gl_GlobalInvocationID.x);
    imageStore(answers, texel * 2 + 0, imageLoad(narrow, texel));
    imageStore(answers, texel * 2 + 1, texelFetch(sampled, texel));
}
)");
            if (program == 0) {
                glDeleteBuffers(1, &buffer);
                glDeleteBuffers(1, &answerBuffer);
                return;
            }

            BindImage(kNarrowUnit, texture, GL_RG32F, GL_READ_ONLY);
            BindImage(kWideUnit, answerTexture, GL_RGBA32F, GL_WRITE_ONLY);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_BUFFER, texture);
            glUseProgram(program);
            glUniform1i(glGetUniformLocation(program, "sampled"), 0);
            glDispatchCompute(kBufferTexels, 1, 1);
            glMemoryBarrier(GL_ALL_BARRIER_BITS);
            EXPECT_EQ(FirstGLError(), 0u) << "the dispatch leaked a GL error";
            glUseProgram(0);

            std::vector<float> readback(answerSeed.size(), -54321.0f);
            glBindBuffer(GL_TEXTURE_BUFFER, answerBuffer);
            glGetBufferSubData(GL_TEXTURE_BUFFER, 0,
                               static_cast<GLsizeiptr>(readback.size() * sizeof(float)), readback.data());
            EXPECT_EQ(FirstGLError(), 0u) << "reading the answers back errored";

            for (int texel = 0; texel < kBufferTexels; ++texel) {
                const float red = static_cast<float>(texel * 10 + 1);
                const float green = static_cast<float>(texel * 10 + 2);
                const std::size_t viaImage = static_cast<std::size_t>(texel) * 8u;
                const std::size_t viaSampler = viaImage + 4u;
                EXPECT_FLOAT_EQ(readback[viaImage + 0u], red) << "texel " << texel << " imageLoad red";
                EXPECT_FLOAT_EQ(readback[viaImage + 1u], green) << "texel " << texel << " imageLoad green";
                EXPECT_FLOAT_EQ(readback[viaSampler + 0u], red) << "texel " << texel << " texelFetch red";
                EXPECT_FLOAT_EQ(readback[viaSampler + 1u], green)
                    << "texel " << texel
                    << " texelFetch green: a samplerBuffer must see whole texels even where the "
                       "image side of the same texture was split";
                EXPECT_FLOAT_EQ(readback[viaSampler + 2u], 0.0f) << "texel " << texel << " texelFetch blue";
                EXPECT_FLOAT_EQ(readback[viaSampler + 3u], 1.0f) << "texel " << texel << " texelFetch alpha";
            }

            glBindBuffer(GL_TEXTURE_BUFFER, 0);
            glDeleteBuffers(1, &buffer);
            glDeleteBuffers(1, &answerBuffer);
            while (glGetError() != GL_NO_ERROR) {
            }
        }

        // The other consumer of the same texture. A widened texture's ES storage really does have
        // four channels, so a sampler reading it raw would see whatever the carrier holds; the
        // logical format's missing channels have to keep reading 0 and 1 (which Espryt arranges
        // with GL_TEXTURE_SWIZZLE_B/A composed under the application's own swizzle). texelFetch
        // rather than a draw, so the case stays a compute dispatch and turns on nothing but the
        // sampled result.
        TEST_F(NonCoreImageFormatScenario, ATwoChannelImageTextureStillSamplesAsRGZeroOne) {
            if (!Ready()) GTEST_SKIP() << "no GL context";
            if (!ImagesAreUsable()) GTEST_SKIP() << "no image load/store on this driver";

            const std::vector<float> seed(static_cast<std::size_t>(kExtent) * kExtent * 2u, -1.0f);
            const std::vector<float> wideSeed(static_cast<std::size_t>(kExtent) * kExtent * 4u, -1.0f);
            const GLuint narrow = MakeTexture(GL_RG32F, GL_RG, GL_FLOAT, seed.data());
            const GLuint wide = MakeTexture(GL_RGBA32F, GL_RGBA, GL_FLOAT, wideSeed.data());
            if (narrow == 0 || wide == 0) return;

            const GLuint storeProgram = MakeComputeProgram(R"(#version 430 core

layout (local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout (rg32f, binding = 0) writeonly uniform image2D narrow;

void main()
{
    imageStore(narrow, ivec2(gl_GlobalInvocationID.xy), vec4(1.0, 2.0, 3.0, 4.0));
}
)");
            const GLuint sampleProgram = MakeComputeProgram(R"(#version 430 core

layout (local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

uniform sampler2D narrowSampler;
layout (rgba32f, binding = 1) writeonly uniform image2D wide;

void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    imageStore(wide, coord, texelFetch(narrowSampler, coord, 0));
}
)");
            if (storeProgram == 0 || sampleProgram == 0) return;

            BindImage(kNarrowUnit, narrow, GL_RG32F, GL_WRITE_ONLY);
            Dispatch(storeProgram);

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, narrow);
            glUseProgram(sampleProgram);
            const GLint samplerLocation = glGetUniformLocation(sampleProgram, "narrowSampler");
            ASSERT_GE(samplerLocation, 0) << "the sampler uniform was not reflected";
            glUniform1i(samplerLocation, 0);
            ASSERT_EQ(FirstGLError(), 0u) << "assigning the texture unit errored";
            glUseProgram(0);

            BindImage(kWideUnit, wide, GL_RGBA32F, GL_WRITE_ONLY);
            Dispatch(sampleProgram);

            const std::vector<float> sampled = ReadFloats(wide, GL_RGBA, 4);
            for (int texel = 0; texel < kExtent * kExtent; ++texel) {
                EXPECT_FLOAT_EQ(sampled[texel * 4 + 0], 1.0f) << "texel " << texel << " red";
                EXPECT_FLOAT_EQ(sampled[texel * 4 + 1], 2.0f) << "texel " << texel << " green";
                EXPECT_FLOAT_EQ(sampled[texel * 4 + 2], 0.0f)
                    << "texel " << texel << ": sampling a two-channel format must report 0 for blue";
                EXPECT_FLOAT_EQ(sampled[texel * 4 + 3], 1.0f)
                    << "texel " << texel << ": sampling a format without alpha must report 1";
            }
        }

    } // namespace
} // namespace MGITest
