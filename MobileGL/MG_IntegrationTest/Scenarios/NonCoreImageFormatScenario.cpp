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
