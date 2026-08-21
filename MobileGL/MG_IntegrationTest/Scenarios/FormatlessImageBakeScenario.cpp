// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/FormatlessImageBakeScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - A FORMAT-LESS IMAGE UNIFORM WHOSE UNIT HOLDS A NON-CORE FORMAT.
//
// GLSL 4.20 lets a write-only image uniform omit its layout format; GLSL ES demands one, so
// DirectGLES BAKES the format of whatever glBindImageTexture put on the unit into the
// declaration. When that format is outside the GLSL ES core thirteen, the bake alone is not
// enough - the baked declaration then has to go through the same channel-widening
// WidenImageFormatsForEssl gives a DECLARED non-core format (see NonCoreImageFormatScenario for
// the widening itself).
//
// The two routes had different arming. The declared route armed the widening on the format
// alone; the baked route armed it only when the driver lacked GL_NV_image_formats. That reads
// like an optimisation and is not one: SPIRV-Cross throws for its is_desktop_only_format set the
// moment it targets ESSL, whatever the driver would have accepted, so on a driver that HAS the
// extension the shader half of the widening stayed switched off while TextureImpl's storage/bind
// half - which keys on SpirvCrossCanPrintEsslImageFormat, not on the driver bit - still ran. The
// stage threw, the program linked without it, and every dispatch silently did nothing.
//
// KHR-GL43.stencil_texturing.functional is where it surfaced: its compute half writes through a
// format-less `uimage2D` bound to an R8UI texture, and returned zeros for every texel.
//
// DISCRIMINATING ONLY WHERE THE DRIVER ADVERTISES GL_NV_image_formats - Mesa does, which is what
// the software lanes run and where this was found. On Adreno 830 and both Malis the extension is
// absent, the old code already armed the widening, and these cases pass before and after; they
// are kept running there as a guard against the opposite mistake.

#include <cstdint>
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

        constexpr int kExtent = 8;

        // No layout format on uni_image on purpose: that is the whole subject. uni_source is a
        // plain integer texture so nothing but the image declaration is in play.
        const char* const kComputeSource = R"(#version 430 core
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
writeonly uniform uimage2D uni_image;
uniform usampler2D uni_source;
void main()
{
    ivec2 at = ivec2(gl_GlobalInvocationID.xy);
    imageStore(uni_image, at, uvec4(texelFetch(uni_source, at, 0).r, 0u, 0u, 0u));
}
)";

        class FormatlessImageBakeScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                if (!BackendHostsCompute()) {
                    GTEST_SKIP() << "no compute stage on " << Gl().BackendName() << " ("
                                 << Gl().RendererString() << ")";
                }
            }

            static bool BackendHostsCompute() {
                GLint maxImageUnits = 0;
                glGetIntegerv(GL_MAX_IMAGE_UNITS, &maxImageUnits);
                DrainErrors();
                return maxImageUnits >= 2;
            }

            static void DrainErrors() {
                for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {
                }
            }

            static GLuint BuildCompute(const char* source, std::string& log) {
                const GLuint cs = glCreateShader(GL_COMPUTE_SHADER);
                glShaderSource(cs, 1, &source, nullptr);
                glCompileShader(cs);
                GLint ok = 0;
                glGetShaderiv(cs, GL_COMPILE_STATUS, &ok);
                if (!ok) {
                    char buffer[2048] = "";
                    glGetShaderInfoLog(cs, sizeof(buffer), nullptr, buffer);
                    log = buffer;
                    glDeleteShader(cs);
                    return 0;
                }
                const GLuint program = glCreateProgram();
                glAttachShader(program, cs);
                glLinkProgram(program);
                glGetProgramiv(program, GL_LINK_STATUS, &ok);
                glDeleteShader(cs);
                if (!ok) {
                    char buffer[2048] = "";
                    glGetProgramInfoLog(program, sizeof(buffer), nullptr, buffer);
                    log = buffer;
                    glDeleteProgram(program);
                    return 0;
                }
                return program;
            }

            // internalFormat is the NON-CORE image format under test; the destination texture and
            // the glBindImageTexture argument both use it, and the shader declares nothing.
            void RunCopy(GLenum internalFormat, GLenum uploadFormat, GLenum uploadType) {
                std::vector<GLuint> expected(kExtent * kExtent);
                for (int i = 0; i < kExtent * kExtent; ++i) {
                    expected[i] = static_cast<GLuint>(1 + i);
                }

                // Source: a core-format integer texture holding 1..64.
                std::vector<GLubyte> sourceBytes(kExtent * kExtent);
                for (int i = 0; i < kExtent * kExtent; ++i) {
                    sourceBytes[i] = static_cast<GLubyte>(expected[i]);
                }
                GLuint sourceTexture = 0;
                glGenTextures(1, &sourceTexture);
                glBindTexture(GL_TEXTURE_2D, sourceTexture);
                glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8UI, kExtent, kExtent);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kExtent, kExtent, GL_RED_INTEGER, GL_UNSIGNED_BYTE,
                                sourceBytes.data());
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

                // Destination: the format under test, zero-filled so "the dispatch did nothing"
                // and "the dispatch wrote zeros" are the same observation the CTS made.
                GLuint destTexture = 0;
                glGenTextures(1, &destTexture);
                glBindTexture(GL_TEXTURE_2D, destTexture);
                glTexStorage2D(GL_TEXTURE_2D, 1, internalFormat, kExtent, kExtent);
                const std::vector<GLubyte> zeros(static_cast<std::size_t>(kExtent) * kExtent * 8, 0);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kExtent, kExtent, uploadFormat, uploadType, zeros.data());
                ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "destination storage";

                std::string log;
                const GLuint program = BuildCompute(kComputeSource, log);
                ASSERT_NE(program, 0u) << "the format-less image program did not build: " << log;

                glUseProgram(program);
                glBindImageTexture(1, destTexture, 0, GL_FALSE, 0, GL_WRITE_ONLY, internalFormat);
                glUniform1i(glGetUniformLocation(program, "uni_image"), 1);
                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_2D, sourceTexture);
                glUniform1i(glGetUniformLocation(program, "uni_source"), 1);
                ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "binding";

                glDispatchCompute(kExtent, kExtent, 1);
                glMemoryBarrier(GL_ALL_BARRIER_BITS);
                EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "dispatch";

                std::vector<GLuint> readback(kExtent * kExtent, 0xFFFFFFFFu);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, destTexture);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, readback.data());
                EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "readback";

                int offenders = 0;
                for (int i = 0; i < kExtent * kExtent; ++i) {
                    if (readback[i] != expected[i]) ++offenders;
                }
                EXPECT_EQ(offenders, 0) << "the dispatch wrote " << offenders << " of "
                                        << (kExtent * kExtent) << " texels wrongly; texel 0 was "
                                        << readback[0] << ", expected " << expected[0]
                                        << ". A whole stage lost to the ESSL emitter looks exactly like this.";

                glUseProgram(0);
                glDeleteProgram(program);
                glDeleteTextures(1, &sourceTexture);
                glDeleteTextures(1, &destTexture);
                DrainErrors();
            }
        };

        // R8UI: one of the seven formats GLSL ES reaches only through GL_NV_image_formats AND one
        // SPIRV-Cross refuses to print for ESSL, so it needs the widening in both driver modes.
        TEST_F(FormatlessImageBakeScenario, R8uiBakedFromTheBoundUnitStillReachesTheDriver) {
            if (!Ready()) GTEST_SKIP();
            RunCopy(GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE);
        }

        // R16UI, from the same set, carried in RGBA16UI: the fix must not be R8UI-shaped.
        TEST_F(FormatlessImageBakeScenario, R16uiBakedFromTheBoundUnitStillReachesTheDriver) {
            if (!Ready()) GTEST_SKIP();
            RunCopy(GL_R16UI, GL_RED_INTEGER, GL_UNSIGNED_SHORT);
        }

        // The control: R32UI is in the GLSL ES core thirteen, so it is baked and never widened.
        // It passed before the fix and has to keep passing.
        TEST_F(FormatlessImageBakeScenario, CoreFormatBakedFromTheBoundUnitIsUnaffected) {
            if (!Ready()) GTEST_SKIP();
            RunCopy(GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT);
        }

    } // namespace
} // namespace MGITest
