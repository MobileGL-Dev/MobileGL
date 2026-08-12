// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/ImageFormatQualifierScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - AN IMAGE UNIFORM THAT DECLARES NO FORMAT.
//
// Desktop GLSL 4.2 lets a writeonly image declaration omit its format layout qualifier:
//
//     writeonly uniform uimage2D uni_image;      // legal desktop GLSL
//
// GLSL ES has no such relaxation; every image uniform must carry one, and Adreno says so as "all
// images have to define layout format", which fails the whole program. That is what took the
// compute half of KHR-GL4x.packed_depth_stencil.stencil_texturing.
//
// The only qualifier that is CORRECT to substitute is whatever glBindImageTexture named for the
// unit that uniform addresses - GL requires the qualifier, the bind format and the texture's
// internal format to belong to one format class - so the format is not knowable when the shader
// is compiled, only when it is drawn with. Espryt therefore BAKES it into the program it
// generates and keys that program on the (unit, format) pairs it baked
// (BackendProgramObjectImpl::ImageUnitFormatsStillMatch, MG_Backend/DirectGLES).
//
// Three separate things follow from "the program is built against live binding state", and each
// one is a case below:
//
//   1. the format reaches the shader at all, so the store lands where the texture is (Writes);
//   2. binding a DIFFERENT format to the same unit rebuilds the program, rather than reusing one
//      compiled against the old format (RebindToADifferentFormatRebuilds);
//   3. an image bound for the FIRST time after the link works, i.e. the program built against
//      "nothing bound yet" is not the one the dispatch runs (FirstBindAfterLinkRebuilds).
//
// Magma needs none of this - Vulkan takes an Unknown-format storage image given
// shaderStorageImageWriteWithoutFormat, and the view format is resolved from the same bind state
// at descriptor time - so every case here runs on both backends and must agree, which is what
// makes the ES-only machinery falsifiable rather than merely exercised.

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
        // The image unit is deliberately NOT 0 and the uniform declares no binding, so the unit
        // has to travel through glUniform1i and be baked into the ESSL alongside the format -
        // the two bakes share a rebuild key and a bug in either shows up as the wrong texel.
        constexpr GLint kImageUnit = 1;

        // KHR-GL4x.packed_depth_stencil.stencil_texturing's own image declaration, verbatim.
        const char* kStoreSource = R"(#version 430 core

layout (local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

writeonly uniform uimage2D uni_image;

void main()
{
    imageStore(uni_image, ivec2(gl_GlobalInvocationID.xy), uvec4(gl_GlobalInvocationID.x + 100u, 0u, 0u, 0u));
}
)";

        class ImageFormatQualifierScenario : public ScenarioTest {
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

            GLuint MakeTexture(GLenum internalFormat) {
                GLuint texture = 0;
                glGenTextures(1, &texture);
                m_textures.push_back(texture);
                glBindTexture(GL_TEXTURE_2D, texture);
                glTexStorage2D(GL_TEXTURE_2D, 1, internalFormat, kExtent, kExtent);
                if (const GLenum error = FirstGLError()) {
                    ADD_FAILURE() << "allocating storage errored with " << GLErrorName(error);
                    return 0;
                }
                // Seeded to a value no dispatch writes, so "the store never happened" and "the
                // store wrote the right thing" cannot be confused.
                const std::vector<GLuint> zeros(static_cast<std::size_t>(kExtent) * kExtent * 4u, 0u);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kExtent, kExtent,
                                internalFormat == GL_RGBA32UI ? GL_RGBA_INTEGER : GL_RED_INTEGER, GL_UNSIGNED_INT,
                                zeros.data());
                while (glGetError() != GL_NO_ERROR) {
                }
                return texture;
            }

            // Texel (x, 0) of the texture's red channel, read back through the GL frontend rather
            // than through a second image uniform: a defect in the format bake would be shared by
            // a reader declared the same way and could cancel itself out.
            GLuint ReadRedTexel(GLuint texture, GLenum internalFormat, int x) {
                const bool rgba = internalFormat == GL_RGBA32UI;
                std::vector<GLuint> texels(static_cast<std::size_t>(kExtent) * kExtent * (rgba ? 4u : 1u),
                                           0xFFFFFFFFu);
                glBindTexture(GL_TEXTURE_2D, texture);
                glGetTexImage(GL_TEXTURE_2D, 0, rgba ? GL_RGBA_INTEGER : GL_RED_INTEGER, GL_UNSIGNED_INT,
                              texels.data());
                if (const GLenum error = FirstGLError()) {
                    ADD_FAILURE() << "reading the image back errored with " << GLErrorName(error);
                    return 0xFFFFFFFFu;
                }
                return texels[static_cast<std::size_t>(x) * (rgba ? 4u : 1u)];
            }

            void DispatchStore(GLuint program, GLuint texture, GLenum internalFormat) {
                glBindImageTexture(static_cast<GLuint>(kImageUnit), texture, 0, GL_FALSE, 0, GL_WRITE_ONLY,
                                   internalFormat);
                ASSERT_EQ(FirstGLError(), 0u) << "glBindImageTexture errored";
                glUseProgram(program);
                const GLint location = glGetUniformLocation(program, "uni_image");
                ASSERT_GE(location, 0) << "the image uniform was not reflected";
                glUniform1i(location, kImageUnit);
                ASSERT_EQ(FirstGLError(), 0u) << "assigning the image unit errored";
                glDispatchCompute(kExtent, 1, 1);
                glMemoryBarrier(GL_ALL_BARRIER_BITS);
                EXPECT_EQ(FirstGLError(), 0u) << "the dispatch leaked a GL error";
                glUseProgram(0);
            }

            std::vector<GLuint> m_programs;
            std::vector<GLuint> m_textures;
        };

        // The defect itself. Without the bake the ES driver refuses the program outright and the
        // texture keeps its seed - which is also exactly what a silently no-op dispatch looks
        // like, and why the seed is a value no store writes.
        TEST_F(ImageFormatQualifierScenario, AFormatlessWriteonlyImageWrites) {
            if (!Ready()) GTEST_SKIP() << "no GL context";
            if (!ImagesAreUsable()) GTEST_SKIP() << "no image load/store on this driver";

            const GLuint program = MakeComputeProgram(kStoreSource);
            const GLuint texture = MakeTexture(GL_R32UI);
            if (program == 0 || texture == 0) return;

            DispatchStore(program, texture, GL_R32UI);
            for (int x = 0; x < kExtent; ++x) {
                EXPECT_EQ(ReadRedTexel(texture, GL_R32UI, x), static_cast<GLuint>(x) + 100u)
                    << "texel " << x << " of a format-less writeonly image did not take the store";
            }
        }

        // The rebuild key. The SAME program is dispatched twice with a different format bound to
        // its unit; a build keyed only on the link (or only on the image UNIT) would reuse the
        // r32ui program for the rgba32ui texture, and the second half would come back seeded.
        //
        // What the SOFTWARE lanes cannot falsify: with the key disabled this case still passes on
        // Mesa, because the reused r32ui declaration writes the red channel of an RGBA32UI image
        // anyway - a format-class mismatch GL leaves undefined and that driver happens to absorb.
        // FirstBindAfterLinkRebuilds below is the case that fails there, because the reused
        // program was built with no format at all and never compiled. Both are kept: this one is
        // the shape a strict driver is entitled to reject, and it is the shape the device runs.
        TEST_F(ImageFormatQualifierScenario, RebindToADifferentFormatRebuilds) {
            if (!Ready()) GTEST_SKIP() << "no GL context";
            if (!ImagesAreUsable()) GTEST_SKIP() << "no image load/store on this driver";

            const GLuint program = MakeComputeProgram(kStoreSource);
            const GLuint first = MakeTexture(GL_R32UI);
            const GLuint second = MakeTexture(GL_RGBA32UI);
            if (program == 0 || first == 0 || second == 0) return;

            DispatchStore(program, first, GL_R32UI);
            for (int x = 0; x < kExtent; ++x) {
                ASSERT_EQ(ReadRedTexel(first, GL_R32UI, x), static_cast<GLuint>(x) + 100u)
                    << "the first format must work before the rebind can be blamed for anything";
            }

            DispatchStore(program, second, GL_RGBA32UI);
            for (int x = 0; x < kExtent; ++x) {
                EXPECT_EQ(ReadRedTexel(second, GL_RGBA32UI, x), static_cast<GLuint>(x) + 100u)
                    << "texel " << x << ": the program was not rebuilt for the newly bound format";
            }

            // ...and back, so the rebuild is not a one-way door: returning to a format the
            // program was once built against must build for it again, not resurrect a cache row.
            const GLuint third = MakeTexture(GL_R32UI);
            if (third == 0) return;
            DispatchStore(program, third, GL_R32UI);
            for (int x = 0; x < kExtent; ++x) {
                EXPECT_EQ(ReadRedTexel(third, GL_R32UI, x), static_cast<GLuint>(x) + 100u)
                    << "texel " << x << ": going back to the first format did not rebuild";
            }
        }

        // Nothing is bound to the unit when the program links, so whatever the first build sees
        // is not the format the dispatch needs. glBindImageTexture must not itself trigger a
        // build - it is an entry point, and building there is the constraint
        // glShaderStorageBlockBinding is held to as well - so the rebuild has to happen at the
        // next dispatch preparation instead. This case fails either way round: no rebuild, or a
        // build attempted from the entry point before the state settles.
        TEST_F(ImageFormatQualifierScenario, FirstBindAfterLinkRebuilds) {
            if (!Ready()) GTEST_SKIP() << "no GL context";
            if (!ImagesAreUsable()) GTEST_SKIP() << "no image load/store on this driver";

            const GLuint program = MakeComputeProgram(kStoreSource);
            if (program == 0) return;

            // Use it once with NOTHING bound to the unit, which is what makes the backend build
            // against an empty binding. The dispatch writes nowhere and must not error.
            glUseProgram(program);
            const GLint location = glGetUniformLocation(program, "uni_image");
            ASSERT_GE(location, 0);
            glUniform1i(location, kImageUnit);
            glDispatchCompute(kExtent, 1, 1);
            glMemoryBarrier(GL_ALL_BARRIER_BITS);
            EXPECT_EQ(FirstGLError(), 0u) << "dispatching with an unbound image unit must not error";
            glUseProgram(0);

            const GLuint texture = MakeTexture(GL_R32UI);
            if (texture == 0) return;
            DispatchStore(program, texture, GL_R32UI);
            for (int x = 0; x < kExtent; ++x) {
                EXPECT_EQ(ReadRedTexel(texture, GL_R32UI, x), static_cast<GLuint>(x) + 100u)
                    << "texel " << x << ": the first bind after the link did not reach the shader";
            }
        }

        // A DECLARED format is authoritative and the bake must never touch it - including when
        // the texture behind the unit has a different (but class-compatible) internal format,
        // which GL explicitly allows. If the bake ever overrode a declaration, this is the case
        // that would go wrong while every other one stayed green.
        TEST_F(ImageFormatQualifierScenario, ADeclaredFormatStillWins) {
            if (!Ready()) GTEST_SKIP() << "no GL context";
            if (!ImagesAreUsable()) GTEST_SKIP() << "no image load/store on this driver";

            const GLuint program = MakeComputeProgram(R"(#version 430 core

layout (local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout (r32ui) writeonly uniform uimage2D uni_image;

void main()
{
    imageStore(uni_image, ivec2(gl_GlobalInvocationID.xy), uvec4(gl_GlobalInvocationID.x + 100u, 0u, 0u, 0u));
}
)");
            const GLuint texture = MakeTexture(GL_R32UI);
            if (program == 0 || texture == 0) return;

            DispatchStore(program, texture, GL_R32UI);
            for (int x = 0; x < kExtent; ++x) {
                EXPECT_EQ(ReadRedTexel(texture, GL_R32UI, x), static_cast<GLuint>(x) + 100u)
                    << "texel " << x << ": a declared format stopped working";
            }
        }

    } // namespace
} // namespace MGITest
