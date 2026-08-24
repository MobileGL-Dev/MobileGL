// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/UnboundImageDescriptorScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - A PROGRAM DECLARES AN IMAGE-BACKED RESOURCE AND THE APPLICATION BINDS NOTHING.
//
// The sibling of GuiBatchScenario's MeshesBlockLeftUnbound, one descriptor kind further out.
// That one pinned an unbound shader storage BLOCK; the same "nothing is bound, so lose the
// whole draw" shape survived in the three image-backed kinds:
//
//   * `samplerBuffer`  - a texture unit with no buffer texture on it, and a buffer texture with
//                        no GL buffer attached to it. Both make the sampler INCOMPLETE (GL 4.6
//                        core 8.9, 8.24), and sampling an incomplete texture returns undefined
//                        VALUES. It is not an error and it is not a lost draw.
//   * `imageBuffer`    - an image unit with nothing on it. GL 4.6 core 8.26 is explicit: loads
//                        return zero and stores are discarded.
//   * `image2D`        - the same rule, through a VkImageView rather than a VkBufferView.
//
// Vulkan has no such thing as an unwritten descriptor, so DirectVulkan's descriptor resolution
// used to answer "no valid descriptor" and both SetupDraw and DispatchCompute skip everything on
// that answer - the draw or dispatch simply never happened, silently. Every test below asserts
// on the OTHER work in the same shader: the pixels the fragment stage painted, or the buffer the
// dispatch filled. All of it is unrelated to the unbound resource and all of it disappeared.
//
// The unbound resource is STATICALLY USED in every case, because an unreferenced one is
// optimised out before it ever reaches a descriptor and would prove nothing. Where the use is a
// read it sits behind a uniform-controlled branch that is false at runtime - the descriptor is
// declared and must be written, but no undefined value reaches an assertion. Where it is a write
// (the `writeonly` cases, which is how the real workloads spell it) it is unconditional: GL says
// the store is discarded, so there is nothing to guard against.
//
// Reproduces on DirectVulkan only. DirectGLES forwards the unbound unit to the GLES driver,
// which does what GL says, so it is the control - every test here must stay green on both.

#include <cstdint>
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

        constexpr int kFboSize = 32;
        constexpr int kElements = 4;

        // No vertex attributes: the quad's corners come from gl_VertexID, so nothing about the
        // vertex fetch can be confused with the descriptor question under test.
        constexpr const char* kQuadVertexSource = R"(#version 430 core
void main() {
    vec2 corner = vec2((gl_VertexID & 1) == 0 ? -1.0 : 1.0,
                       (gl_VertexID & 2) == 0 ? -1.0 : 1.0);
    gl_Position = vec4(corner, 0.0, 1.0);
}
)";

        // The assertion in every draw case: opaque green everywhere. The unbound resource
        // contributes nothing to it - u_readUnbound is 0, so the fetch never runs - but the
        // descriptor for it still has to exist, which is the point.
        constexpr const char* kSamplerBufferFragmentSource = R"(#version 430 core
uniform samplerBuffer u_unbound;
uniform int u_readUnbound;
out vec4 o_color;
void main() {
    vec4 color = vec4(0.0, 1.0, 0.0, 1.0);
    if (u_readUnbound != 0) {
        color = texelFetch(u_unbound, 0);
    }
    o_color = color;
}
)";

        constexpr const char* kSamplerBufferComputeSource = R"(#version 430 core
layout(local_size_x = 1) in;
layout(std430, binding = 0) buffer Output { uint g_data[]; };
uniform samplerBuffer u_unbound;
uniform int u_readUnbound;
void main() {
    uint index = gl_GlobalInvocationID.x;
    uint value = index + 1u;
    if (u_readUnbound != 0) {
        value += uint(texelFetch(u_unbound, 0).r);
    }
    g_data[index] = value;
}
)";

        // writeonly, and the store is unconditional: this is how AcceleratedRendering and the
        // conformance cases spell an image the shader only produces into. GL discards the store
        // when the unit is empty; nothing here reads it back.
        constexpr const char* kImageBufferFragmentSource = R"(#version 430 core
layout(binding = 0, r32ui) uniform writeonly uimageBuffer u_unbound;
out vec4 o_color;
void main() {
    imageStore(u_unbound, 0, uvec4(7u));
    o_color = vec4(0.0, 1.0, 0.0, 1.0);
}
)";

        constexpr const char* kImageBufferComputeSource = R"(#version 430 core
layout(local_size_x = 1) in;
layout(std430, binding = 0) buffer Output { uint g_data[]; };
layout(binding = 0, r32ui) uniform writeonly uimageBuffer u_unbound;
void main() {
    uint index = gl_GlobalInvocationID.x;
    imageStore(u_unbound, int(index), uvec4(7u));
    g_data[index] = index + 1u;
}
)";

        constexpr const char* kImage2DFragmentSource = R"(#version 430 core
layout(binding = 0, rgba8) uniform writeonly image2D u_unbound;
out vec4 o_color;
void main() {
    imageStore(u_unbound, ivec2(0, 0), vec4(1.0));
    o_color = vec4(0.0, 1.0, 0.0, 1.0);
}
)";

        constexpr const char* kImage2DComputeSource = R"(#version 430 core
layout(local_size_x = 1) in;
layout(std430, binding = 0) buffer Output { uint g_data[]; };
layout(binding = 0, rgba8) uniform writeonly image2D u_unbound;
void main() {
    uint index = gl_GlobalInvocationID.x;
    imageStore(u_unbound, ivec2(int(index), 0), vec4(1.0));
    g_data[index] = index + 1u;
}
)";

        // No layout format at all, which GLSL 4.20 allows for a write-only image. The reflection
        // then carries NO format for the binding, so the placeholder descriptor can only be
        // constrained by the declaration's numeric class - a different route through the fix than
        // every typed case above.
        constexpr const char* kFormatlessImage2DComputeSource = R"(#version 430 core
layout(local_size_x = 1) in;
layout(std430, binding = 0) buffer Output { uint g_data[]; };
layout(binding = 0) uniform writeonly image2D u_unbound;
void main() {
    uint index = gl_GlobalInvocationID.x;
    imageStore(u_unbound, ivec2(int(index), 0), vec4(1.0));
    g_data[index] = index + 1u;
}
)";

        class UnboundImageDescriptorScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                m_target = MakeColorFbo(kFboSize, kFboSize);
                ASSERT_NE(m_target.fbo, 0u) << "could not create the render target";
                glGenVertexArrays(1, &m_vao);
                glGenBuffers(1, &m_storage);
                // The harness shares one context across every scenario in the process, so an
                // earlier one may well have left a texture on unit 0 or an image on unit 0. The
                // whole subject here is that nothing is bound, so say so rather than assume it.
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_BUFFER, 0);
                glBindTexture(GL_TEXTURE_2D, 0);
                glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA8);
                FirstGLError();
            }

            void TearDown() override {
                if (!Ready()) return;
                glUseProgram(0);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
                if (m_program != 0) glDeleteProgram(m_program);
                if (m_storage != 0) glDeleteBuffers(1, &m_storage);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                BindDefaultFramebuffer();
                DestroyColorFbo(m_target);
                glViewport(0, 0, Gl().Width(), Gl().Height());
            }

            // Every case needs image or buffer-texture uniforms in a particular stage, and a host
            // that has none of them would report a failure that is about the host, not the fix.
            bool StageSupports(GLenum imageUniformLimit, GLenum textureImageUnitLimit) const {
                GLint images = 0;
                GLint units = 0;
                glGetIntegerv(imageUniformLimit, &images);
                glGetIntegerv(textureImageUnitLimit, &units);
                while (glGetError() != GL_NO_ERROR) {
                }
                return images >= 1 && units >= 1;
            }

            unsigned int MakeComputeProgram(const char* source) {
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
                glAttachShader(program, shader);
                glLinkProgram(program);
                glDeleteShader(shader);
                GLint linked = GL_FALSE;
                glGetProgramiv(program, GL_LINK_STATUS, &linked);
                if (linked == GL_FALSE) {
                    char log[4096] = {};
                    glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
                    ADD_FAILURE() << "the compute program did not link: " << log;
                    glDeleteProgram(program);
                    return 0;
                }
                return program;
            }

            // Fills a four-element SSBO with 1..4 while the unbound resource is declared and
            // statically used. Zeros everywhere mean the dispatch never ran.
            void ExpectDispatchStillRuns(const char* source, const char* what) {
                m_program = MakeComputeProgram(source);
                ASSERT_NE(m_program, 0u);

                const std::vector<unsigned int> zeros(static_cast<std::size_t>(kElements), 0u);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_storage);
                glBufferData(GL_SHADER_STORAGE_BUFFER,
                             static_cast<GLsizeiptr>(zeros.size() * sizeof(unsigned int)), zeros.data(),
                             GL_DYNAMIC_COPY);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_storage);
                ASSERT_EQ(FirstGLError(), 0u) << "setting up the output buffer raised a GL error";

                glUseProgram(m_program);
                const GLint readUnbound = glGetUniformLocation(m_program, "u_readUnbound");
                if (readUnbound != -1) {
                    glUniform1i(readUnbound, 0);
                }
                glDispatchCompute(kElements, 1, 1);
                glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
                EXPECT_EQ(FirstGLError(), 0u) << "the dispatch raised a GL error (" << what << ")";

                std::vector<unsigned int> values(static_cast<std::size_t>(kElements), 0xDEADBEEFu);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_storage);
                glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                                   static_cast<GLsizeiptr>(values.size() * sizeof(unsigned int)), values.data());
                for (int i = 0; i < kElements; ++i) {
                    EXPECT_EQ(values[static_cast<std::size_t>(i)], static_cast<unsigned int>(i + 1))
                        << "element " << i << " came back as " << values[static_cast<std::size_t>(i)]
                        << "; zero everywhere means the whole dispatch was dropped over the unbound " << what;
                }
            }

            // Paints the whole render target green while the unbound resource is declared and
            // statically used. A black target means the draw never happened.
            void ExpectDrawStillRuns(const char* fragmentSource, const char* what) {
                std::string error;
                m_program = CompileProgram(kQuadVertexSource, fragmentSource, &error);
                ASSERT_NE(m_program, 0u) << error;

                BindFbo(m_target);
                ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
                glBindVertexArray(m_vao);
                glUseProgram(m_program);
                const GLint readUnbound = glGetUniformLocation(m_program, "u_readUnbound");
                if (readUnbound != -1) {
                    glUniform1i(readUnbound, 0);
                }
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                glBindVertexArray(0);
                EXPECT_EQ(FirstGLError(), 0u) << "the draw raised a GL error (" << what << ")";

                const Image image = ReadPixels(kFboSize, kFboSize);
                ASSERT_FALSE(image.Empty()) << "the readback came back empty";
                // Whole-region, not a centre pixel: the quad covers the target exactly, so
                // anything short of all of it is a failure worth naming.
                EXPECT_TRUE(RegionIsMostly(image, 0, kFboSize - 1, 0, kFboSize - 1, "green", 0.0,
                                           std::string("the quad drawn with an unbound ") + what))
                    << "an all-black target means the draw was dropped over the unbound " << what;
            }

            ColorFbo m_target{};
            GLuint m_vao = 0;
            GLuint m_storage = 0;
            unsigned int m_program = 0;
        };

    } // namespace

    // ---- uniform samplerBuffer (VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) --------------------

    TEST_F(UnboundImageDescriptorScenario, ADeclaredButUnboundSamplerBufferDoesNotLoseTheDispatch) {
        if (!Ready() || IsSkipped()) return;
        if (!StageSupports(GL_MAX_COMPUTE_IMAGE_UNIFORMS, GL_MAX_COMPUTE_TEXTURE_IMAGE_UNITS)) {
            GTEST_SKIP() << "the compute stage has no texture image units";
        }
        ExpectDispatchStillRuns(kSamplerBufferComputeSource, "samplerBuffer");
    }

    TEST_F(UnboundImageDescriptorScenario, ADeclaredButUnboundSamplerBufferDoesNotLoseTheDraw) {
        if (!Ready() || IsSkipped()) return;
        ExpectDrawStillRuns(kSamplerBufferFragmentSource, "samplerBuffer");
    }

    // The other way a texel-buffer descriptor comes out empty: the unit HAS a buffer texture, but
    // no glTexBuffer ever attached a buffer object to it. GL calls that texture incomplete, which
    // is undefined data and not a lost draw - a separate site in the resolve from the one above,
    // and it used to return false too.
    TEST_F(UnboundImageDescriptorScenario, ABufferTextureWithNoAttachedBufferDoesNotLoseTheDraw) {
        if (!Ready() || IsSkipped()) return;

        GLuint texture = 0;
        glGenTextures(1, &texture);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_BUFFER, texture);
        // Deliberately no glTexBuffer: the texture exists and is bound, and has no store.
        ASSERT_EQ(FirstGLError(), 0u) << "binding an empty buffer texture raised a GL error";

        ExpectDrawStillRuns(kSamplerBufferFragmentSource, "buffer texture with no attached buffer");

        glBindTexture(GL_TEXTURE_BUFFER, 0);
        glDeleteTextures(1, &texture);
    }

    // ---- writeonly imageBuffer (VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) --------------------

    TEST_F(UnboundImageDescriptorScenario, AWriteonlyImageBufferLeftUnboundDoesNotLoseTheDispatch) {
        if (!Ready() || IsSkipped()) return;
        if (!StageSupports(GL_MAX_COMPUTE_IMAGE_UNIFORMS, GL_MAX_COMPUTE_TEXTURE_IMAGE_UNITS)) {
            GTEST_SKIP() << "the compute stage has no image uniforms";
        }
        ExpectDispatchStillRuns(kImageBufferComputeSource, "imageBuffer");
    }

    TEST_F(UnboundImageDescriptorScenario, AWriteonlyImageBufferLeftUnboundDoesNotLoseTheDraw) {
        if (!Ready() || IsSkipped()) return;
        if (!StageSupports(GL_MAX_FRAGMENT_IMAGE_UNIFORMS, GL_MAX_TEXTURE_IMAGE_UNITS)) {
            GTEST_SKIP() << "the fragment stage has no image uniforms";
        }
        ExpectDrawStillRuns(kImageBufferFragmentSource, "imageBuffer");
    }

    // ---- writeonly image2D (VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) -------------------------------

    TEST_F(UnboundImageDescriptorScenario, AWriteonlyImage2DLeftUnboundDoesNotLoseTheDispatch) {
        if (!Ready() || IsSkipped()) return;
        if (!StageSupports(GL_MAX_COMPUTE_IMAGE_UNIFORMS, GL_MAX_COMPUTE_TEXTURE_IMAGE_UNITS)) {
            GTEST_SKIP() << "the compute stage has no image uniforms";
        }
        ExpectDispatchStillRuns(kImage2DComputeSource, "image2D");
    }

    TEST_F(UnboundImageDescriptorScenario, AWriteonlyImage2DLeftUnboundDoesNotLoseTheDraw) {
        if (!Ready() || IsSkipped()) return;
        if (!StageSupports(GL_MAX_FRAGMENT_IMAGE_UNIFORMS, GL_MAX_TEXTURE_IMAGE_UNITS)) {
            GTEST_SKIP() << "the fragment stage has no image uniforms";
        }
        ExpectDrawStillRuns(kImage2DFragmentSource, "image2D");
    }

    TEST_F(UnboundImageDescriptorScenario, AFormatlessWriteonlyImage2DLeftUnboundDoesNotLoseTheDispatch) {
        if (!Ready() || IsSkipped()) return;
        if (!StageSupports(GL_MAX_COMPUTE_IMAGE_UNIFORMS, GL_MAX_COMPUTE_TEXTURE_IMAGE_UNITS)) {
            GTEST_SKIP() << "the compute stage has no image uniforms";
        }
        ExpectDispatchStillRuns(kFormatlessImage2DComputeSource, "format-less image2D");
    }

} // namespace MGITest
