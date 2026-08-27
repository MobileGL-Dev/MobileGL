// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/LayeredAttachmentShapeScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - THE ATTACHMENT SHAPES A LAYERED FRAMEBUFFER CAN TAKE, AND THE ONE VIEW TYPE
// VULKAN ACCEPTS FOR ALL OF THEM.
//
// glFramebufferTexture on a GL_TEXTURE_3D or a GL_TEXTURE_CUBE_MAP_ARRAY makes a LAYERED
// framebuffer: one attachment that covers every slice / layer-face, addressed by a geometry
// shader writing gl_Layer. Vulkan has exactly one legal spelling for that
// (VUID-VkFramebufferCreateInfo-flags-04113: an attachment view must be VK_IMAGE_VIEW_TYPE_2D
// or _2D_ARRAY), and DirectVulkan used to hand vkCreateFramebuffer the IMAGE's own view type
// instead:
//
//   * GL_TEXTURE_3D  -> VK_IMAGE_VIEW_TYPE_3D. A 3D image has arrayLayers == 1 and keeps its
//     layers on z, so the layer-span guard measured [0, depth) against 1, refused, and returned
//     VK_NULL_HANDLE - which then went into pAttachments as a null handle.
//   * GL_TEXTURE_CUBE_MAP_ARRAY -> VK_IMAGE_VIEW_TYPE_CUBE_ARRAY. A perfectly valid view, of a
//     type no framebuffer may take. The driver dereferenced or rejected it inside
//     vkCreateFramebuffer.
//
// Both exits were guarded only by MOBILEGL_ASSERT, which an INFO build (the production and CTS
// default) compiles to nothing - so both were process kills, not wrong pixels: 51 lost QPA
// records over 7 conformance bodies, one runner restart each.
//
// The same function is what routes a NON-layered slice of a 3D texture
// (glFramebufferTextureLayer), and it had the mirror-image hole: it asked for a 3D view there
// too, so the per-slice branch that exists for exactly this case was unreachable and every
// slice above z = 0 came back VK_NULL_HANDLE.
//
// The five cases below are those shapes - layered 3D, one 3D slice, layered cube-map array, and
// its depth and packed depth-stencil attachments - and each one asserts LAYER ROUTING, not merely
// survival: the colour a layer receives is a function of its own index, so an attachment that
// collapsed onto layer 0, or attached one face of a cube, fails on the layers it did not reach
// rather than passing quietly. Every texture is seeded with a poison value first, so "the draw
// never landed here" reads differently from "the wrong layer landed here".
//
// One of them turned out not to be a DirectVulkan bug at all. glFramebufferTexture on
// GL_DEPTH_STENCIL_ATTACHMENT is a shorthand the front end splits into a depth and a stencil
// attachment, and the split dropped the call's `layered` flag - so a layered colour attachment
// sat beside a non-layered depth/stencil one and BOTH backends silently lost the draw. That is
// the shape texture_cube_map_array.stencil_attachments_*_layered and
// geometry_shader.layered_framebuffer.stencil_support are built on, and it is why they fail on
// Espryt as well as crashing on Magma. Case (5) is what found it.
//
// DirectGLES is the control: it hands the same GL calls to the driver, so a red on both backends
// means the scenario is wrong - or the defect is in the shared front end, as it was above - and a
// red on DirectVulkan alone means Magma is.

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
        // Four z slices: enough that "only slice 0 was written" and "the whole thing was written"
        // are different answers, and small enough that the geometry shader stays well inside
        // GL_MAX_GEOMETRY_OUTPUT_VERTICES.
        constexpr int k3DSlices = 4;
        // Two cubes. One cube would let "attached a single cube" pass; twelve layer-faces would
        // not.
        constexpr int kCubeLayerFaces = 12;
        // The slice a non-layered 3D attachment names. Not 0: slice 0 is the one address that is
        // right whether or not the slice is resolved at all.
        constexpr int kSubjectSlice = 2;

        // A colour no pass paints, uploaded before every draw. A layer that reads it back was
        // never rendered to.
        constexpr GLubyte kPoison = 0xAB;

        // What pass `pass` paints on layer `layer`. r and g name the LAYER (so a mis-routed write
        // says which layer it came from) and b names the PASS (so "the second draw was not
        // rejected" is distinguishable from "the first draw never happened").
        Rgba8 ExpectedColor(int layer, int pass) {
            return {static_cast<GLubyte>(10 + layer * 20), static_cast<GLubyte>(200 - layer * 10),
                    static_cast<GLubyte>(3 + pass * 60), 255};
        }

        std::string Describe(const Rgba8& color) {
            return "(" + std::to_string(color.r) + ", " + std::to_string(color.g) + ", " +
                   std::to_string(color.b) + ", " + std::to_string(color.a) + ")";
        }

        // A full-viewport triangle built from gl_VertexID, so nothing here needs a vertex buffer
        // and the draw cannot fail for a reason that has nothing to do with the attachment.
        // u_depth is the NDC z the whole primitive sits at - the depth/stencil case needs two
        // different ones.
        const char* const kVertexSource = R"(#version 420 core
uniform float u_depth;
void main()
{
    vec2 corner = vec2((gl_VertexID == 1) ? 3.0 : -1.0, (gl_VertexID == 2) ? 3.0 : -1.0);
    gl_Position = vec4(corner, u_depth, 1.0);
}
)";

        // The layer count is baked in as a literal rather than passed as a uniform: a
        // non-constant loop bound in a geometry shader is legal but is one more thing the
        // ESSL transpile could get wrong, and this scenario is not about that.
        std::string MakeGeometrySource(int layerCount) {
            return "#version 420 core\n"
                   "layout(triangles) in;\n"
                   "layout(triangle_strip, max_vertices = " +
                   std::to_string(layerCount * 3) +
                   ") out;\n"
                   "flat out int v_layer;\n"
                   "void main()\n"
                   "{\n"
                   "    for (int layer = 0; layer < " +
                   std::to_string(layerCount) +
                   "; ++layer) {\n"
                   "        for (int i = 0; i < 3; ++i) {\n"
                   "            gl_Layer = layer;\n"
                   "            v_layer = layer;\n"
                   "            gl_Position = gl_in[i].gl_Position;\n"
                   "            EmitVertex();\n"
                   "        }\n"
                   "        EndPrimitive();\n"
                   "    }\n"
                   "}\n";
        }

        const char* const kLayeredFragmentSource = R"(#version 420 core
flat in int v_layer;
uniform int u_pass;
out vec4 o_color;
void main()
{
    o_color = vec4(float(10 + v_layer * 20) / 255.0,
                   float(200 - v_layer * 10) / 255.0,
                   float(3 + u_pass * 60) / 255.0,
                   1.0);
}
)";

        // The non-layered case has no geometry stage at all - the slice comes from the
        // attachment, not from gl_Layer - so it names its layer through a uniform.
        const char* const kFlatFragmentSource = R"(#version 420 core
uniform int u_layer;
uniform int u_pass;
out vec4 o_color;
void main()
{
    o_color = vec4(float(10 + u_layer * 20) / 255.0,
                   float(200 - u_layer * 10) / 255.0,
                   float(3 + u_pass * 60) / 255.0,
                   1.0);
}
)";

        class LayeredAttachmentShapeScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
                DrainErrors();
            }

            void TearDown() override {
                if (!Ready()) return;
                glUseProgram(0);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                for (const GLuint fbo : m_fbos) glDeleteFramebuffers(1, &fbo);
                m_fbos.clear();
                for (const GLuint texture : m_textures) glDeleteTextures(1, &texture);
                m_textures.clear();
                for (const GLuint program : m_programs) glDeleteProgram(program);
                m_programs.clear();
                glBindVertexArray(0);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                m_vao = 0;
                glDisable(GL_DEPTH_TEST);
                glDisable(GL_STENCIL_TEST);
                DrainErrors();
            }

            static void DrainErrors() {
                for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {
                }
            }

            // 0 on a DirectGLES driver without GL_EXT_geometry_shader and on a DirectVulkan
            // device without the geometryShader feature. The same probe GeometryDrawModeScenario
            // and IoBlockNameCollisionScenario use.
            static bool BackendHostsGeometry() {
                GLint maxGeometryOutputVertices = 0;
                glGetIntegerv(GL_MAX_GEOMETRY_OUTPUT_VERTICES, &maxGeometryOutputVertices);
                DrainErrors();
                return maxGeometryOutputVertices >= kCubeLayerFaces * 3;
            }

            static std::string InfoLog(GLuint object, bool isShader) {
                GLint length = 0;
                if (isShader) {
                    glGetShaderiv(object, GL_INFO_LOG_LENGTH, &length);
                } else {
                    glGetProgramiv(object, GL_INFO_LOG_LENGTH, &length);
                }
                std::vector<char> buffer(static_cast<std::size_t>(length) + 1, '\0');
                if (isShader) {
                    glGetShaderInfoLog(object, length + 1, nullptr, buffer.data());
                } else {
                    glGetProgramInfoLog(object, length + 1, nullptr, buffer.data());
                }
                return buffer.data();
            }

            // geometrySource may be null, which builds the no-geometry-stage program the
            // non-layered case uses.
            GLuint BuildProgram(const char* geometrySource, const char* fragmentSource) {
                std::vector<GLuint> shaders;
                const auto compile = [&](GLenum stage, const char* source) {
                    const GLuint shader = glCreateShader(stage);
                    glShaderSource(shader, 1, &source, nullptr);
                    glCompileShader(shader);
                    GLint compiled = 0;
                    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
                    shaders.push_back(shader);
                    if (compiled == GL_FALSE) {
                        ADD_FAILURE() << "stage 0x" << std::hex << stage << std::dec
                                      << " did not compile: " << InfoLog(shader, true);
                        return false;
                    }
                    return true;
                };

                bool ok = compile(GL_VERTEX_SHADER, kVertexSource);
                if (ok && geometrySource != nullptr) ok = compile(GL_GEOMETRY_SHADER, geometrySource);
                if (ok) ok = compile(GL_FRAGMENT_SHADER, fragmentSource);
                if (!ok) {
                    for (const GLuint shader : shaders) glDeleteShader(shader);
                    return 0;
                }

                const GLuint program = glCreateProgram();
                for (const GLuint shader : shaders) glAttachShader(program, shader);
                glLinkProgram(program);
                for (const GLuint shader : shaders) glDeleteShader(shader);
                GLint linked = 0;
                glGetProgramiv(program, GL_LINK_STATUS, &linked);
                if (linked == GL_FALSE) {
                    ADD_FAILURE() << "the program did not link: " << InfoLog(program, false);
                    glDeleteProgram(program);
                    return 0;
                }
                m_programs.push_back(program);
                return program;
            }

            GLuint TrackTexture() {
                GLuint texture = 0;
                glGenTextures(1, &texture);
                m_textures.push_back(texture);
                return texture;
            }

            GLuint TrackFramebuffer() {
                GLuint fbo = 0;
                glGenFramebuffers(1, &fbo);
                m_fbos.push_back(fbo);
                return fbo;
            }

            // An RGBA8 3D texture, every texel poisoned.
            GLuint MakePoisoned3DColor() {
                const GLuint texture = TrackTexture();
                glBindTexture(GL_TEXTURE_3D, texture);
                glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexStorage3D(GL_TEXTURE_3D, 1, GL_RGBA8, kExtent, kExtent, k3DSlices);
                const std::vector<GLubyte> seed(
                    static_cast<std::size_t>(kExtent) * kExtent * k3DSlices * 4, kPoison);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, kExtent, kExtent, k3DSlices, GL_RGBA,
                                GL_UNSIGNED_BYTE, seed.data());
                glBindTexture(GL_TEXTURE_3D, 0);
                return texture;
            }

            // An RGBA8 cube-map array of kCubeLayerFaces layer-faces, every texel poisoned.
            GLuint MakePoisonedCubeArrayColor() {
                const GLuint texture = TrackTexture();
                glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, texture);
                glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexStorage3D(GL_TEXTURE_CUBE_MAP_ARRAY, 1, GL_RGBA8, kExtent, kExtent, kCubeLayerFaces);
                const std::vector<GLubyte> seed(
                    static_cast<std::size_t>(kExtent) * kExtent * kCubeLayerFaces * 4, kPoison);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexSubImage3D(GL_TEXTURE_CUBE_MAP_ARRAY, 0, 0, 0, 0, kExtent, kExtent, kCubeLayerFaces,
                                GL_RGBA, GL_UNSIGNED_BYTE, seed.data());
                glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, 0);
                return texture;
            }

            // A depth (or packed depth-stencil) cube-map array of the same shape. No upload: a
            // depth array is filled by clearing through an attachment, which is the state the
            // gating cases start from anyway.
            GLuint MakeCubeArrayDepth(GLenum internalFormat) {
                const GLuint texture = TrackTexture();
                glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, texture);
                glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexStorage3D(GL_TEXTURE_CUBE_MAP_ARRAY, 1, internalFormat, kExtent, kExtent, kCubeLayerFaces);
                glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, 0);
                return texture;
            }

            // glGetTexImage rather than a per-layer glReadPixels: a cube-map array has no
            // per-layer attachment on every backend, and glGetTexImage is the readback both of
            // them answer for whole-level layered targets (LayeredTextureReadbackScenario pins
            // that contract). It is a real GPU readback on DirectVulkan - the texture manager
            // copies the image into a staging buffer - so a stale CPU shadow cannot pass it.
            std::vector<Rgba8> ReadLevel(GLenum target, GLuint texture, int layers) {
                std::vector<Rgba8> texels(static_cast<std::size_t>(kExtent) * kExtent * layers, Rgba8{});
                glBindTexture(target, texture);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glGetTexImage(target, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels.data());
                glBindTexture(target, 0);
                return texels;
            }

            // Every texel of every layer must be that layer's expected colour. Reported per layer
            // so a failure names which one, and the poison is called out by name.
            void ExpectEveryLayer(const std::vector<Rgba8>& texels, int layers, int pass, const char* what) {
                for (int layer = 0; layer < layers; ++layer) {
                    const Rgba8 expected = ExpectedColor(layer, pass);
                    for (int y = 0; y < kExtent; ++y) {
                        for (int x = 0; x < kExtent; ++x) {
                            const std::size_t index =
                                (static_cast<std::size_t>(layer) * kExtent + y) * kExtent + x;
                            const Rgba8 actual = texels[index];
                            if (actual == expected) continue;
                            ADD_FAILURE()
                                << what << ": layer " << layer << " texel (" << x << ", " << y << ") is "
                                << Describe(actual) << ", expected " << Describe(expected)
                                << (actual.r == kPoison && actual.g == kPoison
                                        ? " - the poison, so nothing was ever rendered into this layer"
                                        : "");
                            // One message per layer is enough to say what happened.
                            y = kExtent;
                            break;
                        }
                    }
                }
            }

            ::testing::AssertionResult FramebufferIsComplete() {
                const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
                if (status == GL_FRAMEBUFFER_COMPLETE) return ::testing::AssertionSuccess();
                return ::testing::AssertionFailure() << "framebuffer status 0x" << std::hex << status;
            }

            // One layered pass over the whole attachment.
            void DrawLayered(GLuint program, int pass, float depth) {
                glUseProgram(program);
                const GLint passLocation = glGetUniformLocation(program, "u_pass");
                ASSERT_GE(passLocation, 0) << "u_pass was not reflected";
                glUniform1i(passLocation, pass);
                const GLint depthLocation = glGetUniformLocation(program, "u_depth");
                ASSERT_GE(depthLocation, 0) << "u_depth was not reflected";
                glUniform1f(depthLocation, depth);
                glDrawArrays(GL_TRIANGLES, 0, 3);
            }

            GLuint m_vao = 0;
            std::vector<GLuint> m_textures;
            std::vector<GLuint> m_fbos;
            std::vector<GLuint> m_programs;
        };

        // (1) A LAYERED GL_TEXTURE_3D colour attachment. Pre-fix this is the null VkImageView:
        // the attachment asked for a 3D view, whose [0, 4) layer span was measured against the
        // image's arrayLayers == 1 and refused, and VK_NULL_HANDLE went to vkCreateFramebuffer.
        TEST_F(LayeredAttachmentShapeScenario, LayeredThreeDColorAttachmentReachesEverySlice) {
            if (!Ready()) return;
            if (!BackendHostsGeometry()) GTEST_SKIP() << "no geometry stage: nothing can write gl_Layer";

            const std::string geometrySource = MakeGeometrySource(k3DSlices);
            const GLuint program = BuildProgram(geometrySource.c_str(), kLayeredFragmentSource);
            if (program == 0) return;

            const GLuint color = MakePoisoned3DColor();
            ASSERT_EQ(FirstGLError(), 0u) << "creating the RGBA8 3D texture failed";

            const GLuint fbo = TrackFramebuffer();
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, color, 0);
            glDrawBuffer(GL_COLOR_ATTACHMENT0);
            ASSERT_EQ(FirstGLError(), 0u) << "attaching the 3D texture layered failed";
            ASSERT_TRUE(FramebufferIsComplete());

            glViewport(0, 0, kExtent, kExtent);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_STENCIL_TEST);
            DrawLayered(program, /*pass=*/0, /*depth=*/0.0f);
            EXPECT_EQ(FirstGLError(), 0u) << "the layered draw errored";

            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            const std::vector<Rgba8> texels = ReadLevel(GL_TEXTURE_3D, color, k3DSlices);
            EXPECT_EQ(FirstGLError(), 0u) << "reading the 3D level back errored";
            ExpectEveryLayer(texels, k3DSlices, /*pass=*/0, "layered GL_TEXTURE_3D colour attachment");

            Gl().EndFrame();
        }

        // (2) The same texture attached ONE SLICE at a time, which is the other half of the same
        // view-type decision. Pre-fix a non-layered 3D attachment also asked for a 3D view, so
        // the per-slice branch never ran and slice 2 resolved to VK_NULL_HANDLE. Needs no
        // geometry stage - the slice comes from the attachment.
        TEST_F(LayeredAttachmentShapeScenario, NonLayeredThreeDSliceAttachmentWritesOnlyThatSlice) {
            if (!Ready()) return;

            const GLuint program = BuildProgram(nullptr, kFlatFragmentSource);
            if (program == 0) return;

            const GLuint color = MakePoisoned3DColor();
            ASSERT_EQ(FirstGLError(), 0u) << "creating the RGBA8 3D texture failed";

            const GLuint fbo = TrackFramebuffer();
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, color, 0, kSubjectSlice);
            glDrawBuffer(GL_COLOR_ATTACHMENT0);
            ASSERT_EQ(FirstGLError(), 0u) << "attaching slice " << kSubjectSlice << " failed";
            ASSERT_TRUE(FramebufferIsComplete());

            glViewport(0, 0, kExtent, kExtent);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_STENCIL_TEST);
            glUseProgram(program);
            const GLint layerLocation = glGetUniformLocation(program, "u_layer");
            const GLint passLocation = glGetUniformLocation(program, "u_pass");
            const GLint depthLocation = glGetUniformLocation(program, "u_depth");
            ASSERT_GE(layerLocation, 0);
            ASSERT_GE(passLocation, 0);
            ASSERT_GE(depthLocation, 0);
            glUniform1i(layerLocation, kSubjectSlice);
            glUniform1i(passLocation, 0);
            glUniform1f(depthLocation, 0.0f);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            EXPECT_EQ(FirstGLError(), 0u) << "the per-slice draw errored";

            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            const std::vector<Rgba8> texels = ReadLevel(GL_TEXTURE_3D, color, k3DSlices);
            EXPECT_EQ(FirstGLError(), 0u) << "reading the 3D level back errored";

            const Rgba8 expected = ExpectedColor(kSubjectSlice, 0);
            const Rgba8 poison{kPoison, kPoison, kPoison, kPoison};
            for (int slice = 0; slice < k3DSlices; ++slice) {
                const Rgba8& target = (slice == kSubjectSlice) ? expected : poison;
                for (int y = 0; y < kExtent; ++y) {
                    for (int x = 0; x < kExtent; ++x) {
                        const std::size_t index =
                            (static_cast<std::size_t>(slice) * kExtent + y) * kExtent + x;
                        const Rgba8 actual = texels[index];
                        if (actual == target) continue;
                        ADD_FAILURE() << "slice " << slice << " texel (" << x << ", " << y << ") is "
                                      << Describe(actual) << ", expected " << Describe(target)
                                      << (slice == kSubjectSlice
                                              ? " - the attached slice was not the one written"
                                              : " - a slice the attachment did not name was written");
                        y = kExtent;
                        break;
                    }
                }
            }

            Gl().EndFrame();
        }

        // (3) A LAYERED GL_TEXTURE_CUBE_MAP_ARRAY colour attachment. Pre-fix this is the other
        // exit: a valid CUBE_ARRAY view of a type no framebuffer may take, handed straight to
        // vkCreateFramebuffer.
        TEST_F(LayeredAttachmentShapeScenario, LayeredCubeMapArrayColorAttachmentReachesEveryLayerFace) {
            if (!Ready()) return;
            if (!BackendHostsGeometry()) GTEST_SKIP() << "no geometry stage: nothing can write gl_Layer";

            const GLuint color = MakePoisonedCubeArrayColor();
            if (const GLenum error = FirstGLError()) {
                GTEST_SKIP() << "no usable GL_TEXTURE_CUBE_MAP_ARRAY on this backend: " << GLErrorName(error);
            }

            const std::string geometrySource = MakeGeometrySource(kCubeLayerFaces);
            const GLuint program = BuildProgram(geometrySource.c_str(), kLayeredFragmentSource);
            if (program == 0) return;

            const GLuint fbo = TrackFramebuffer();
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, color, 0);
            glDrawBuffer(GL_COLOR_ATTACHMENT0);
            ASSERT_EQ(FirstGLError(), 0u) << "attaching the cube-map array layered failed";
            ASSERT_TRUE(FramebufferIsComplete());

            glViewport(0, 0, kExtent, kExtent);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_STENCIL_TEST);
            DrawLayered(program, /*pass=*/0, /*depth=*/0.0f);
            EXPECT_EQ(FirstGLError(), 0u) << "the layered draw errored";

            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            const std::vector<Rgba8> texels = ReadLevel(GL_TEXTURE_CUBE_MAP_ARRAY, color, kCubeLayerFaces);
            EXPECT_EQ(FirstGLError(), 0u) << "reading the cube-map-array level back errored";
            ExpectEveryLayer(texels, kCubeLayerFaces, /*pass=*/0,
                             "layered GL_TEXTURE_CUBE_MAP_ARRAY colour attachment");

            Gl().EndFrame();
        }

        // (4) A layered cube-map-array DEPTH attachment, proved to have covered every layer-face:
        //
        //   pass 0 paints at z = 0 against a depth buffer cleared to 1;
        //   pass 1 paints at z = +0.5, which GL_LESS must reject.
        //
        // A layer that reads back pass 1's colour is a layer the depth attachment never covered -
        // which is exactly what attaching one layer-face of it, or none, looks like. Runs on both
        // backends: this is the cross-backend control for the packed case below.
        TEST_F(LayeredAttachmentShapeScenario, LayeredCubeMapArrayDepthAttachmentGatesEveryLayerFace) {
            if (!Ready()) return;
            if (!BackendHostsGeometry()) GTEST_SKIP() << "no geometry stage: nothing can write gl_Layer";

            const GLuint color = MakePoisonedCubeArrayColor();
            if (const GLenum error = FirstGLError()) {
                GTEST_SKIP() << "no usable GL_TEXTURE_CUBE_MAP_ARRAY on this backend: " << GLErrorName(error);
            }

            const GLuint depth = MakeCubeArrayDepth(GL_DEPTH_COMPONENT24);
            if (const GLenum error = FirstGLError()) {
                GTEST_SKIP() << "no depth GL_TEXTURE_CUBE_MAP_ARRAY on this backend: " << GLErrorName(error);
            }

            const std::string geometrySource = MakeGeometrySource(kCubeLayerFaces);
            const GLuint program = BuildProgram(geometrySource.c_str(), kLayeredFragmentSource);
            if (program == 0) return;

            const GLuint fbo = TrackFramebuffer();
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, color, 0);
            glFramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, depth, 0);
            glDrawBuffer(GL_COLOR_ATTACHMENT0);
            ASSERT_EQ(FirstGLError(), 0u) << "attaching the layered colour + depth pair failed";
            ASSERT_TRUE(FramebufferIsComplete());

            glViewport(0, 0, kExtent, kExtent);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_STENCIL_TEST);
            glDepthMask(GL_TRUE);
            glClearDepth(1.0);
            glClear(GL_DEPTH_BUFFER_BIT);
            ASSERT_EQ(FirstGLError(), 0u) << "clearing the layered depth attachment errored";

            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LESS);
            DrawLayered(program, /*pass=*/0, /*depth=*/0.0f);
            DrawLayered(program, /*pass=*/1, /*depth=*/0.5f); // farther: GL_LESS must reject it
            EXPECT_EQ(FirstGLError(), 0u) << "the two layered draws errored";

            glDisable(GL_DEPTH_TEST);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            const std::vector<Rgba8> texels = ReadLevel(GL_TEXTURE_CUBE_MAP_ARRAY, color, kCubeLayerFaces);
            EXPECT_EQ(FirstGLError(), 0u) << "reading the cube-map-array level back errored";
            ExpectEveryLayer(texels, kCubeLayerFaces, /*pass=*/0,
                             "layered cube-map-array depth attachment (pass 1's colour on a layer means the "
                             "depth test did not cover it)");

            Gl().EndFrame();
        }

        // (5) The PACKED depth-stencil shape the conformance suite crashes on:
        // texture_cube_map_array.stencil_attachments_*_layered attaches a cube-map array as COLOR0
        // AND the same-shaped GL_DEPTH24_STENCIL8 array as GL_DEPTH_STENCIL_ATTACHMENT, both
        // layered. Both aspects are proved to have covered every layer-face:
        //
        //   pass 0 paints at z = 0 with the stencil op writing 1;
        //   pass 1 paints at z = +0.5, which the depth test must reject;
        //   pass 2 paints with the depth test off but a stencil func of EQUAL 0, which the
        //          stencil written by pass 0 must reject.
        //
        // The probe in front of the gating is where this scenario earned its keep. The attachment
        // point ITSELF was broken: glFramebufferTexture(GL_DEPTH_STENCIL_ATTACHMENT) is a
        // shorthand that the front end splits into a depth and a stencil attachment, and the split
        // dropped the call's `layered` flag (GL_Framebuffer.cpp,
        // AttachFramebufferTextureWithUploadTarget). A layered colour attachment therefore sat
        // beside a NON-layered depth/stencil one, and both backends lost the draw entirely - with
        // no GL error and glCheckFramebufferStatus answering COMPLETE. DirectVulkan built the
        // depth/stencil view with layerCount 1 under a framebuffer declaring 12 layers
        // (VUID-VkFramebufferCreateInfo-flags-04535, which the validation layers report on this
        // exact case); DirectGLES attached one layer of it beside a layered colour target, which
        // the driver answers with GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS. Case (4) above is what
        // isolates it to the attachment point: the same cube-map array on GL_DEPTH_ATTACHMENT
        // rendered and gated correctly throughout.
        //
        // So the probe stays, as an assertion rather than as scaffolding: it turns that regression
        // back into ONE message about the shape instead of twelve about individual layers.
        TEST_F(LayeredAttachmentShapeScenario, LayeredCubeMapArrayDepthStencilAttachmentGatesEveryLayerFace) {
            if (!Ready()) return;
            if (!BackendHostsGeometry()) GTEST_SKIP() << "no geometry stage: nothing can write gl_Layer";

            const GLuint color = MakePoisonedCubeArrayColor();
            if (const GLenum error = FirstGLError()) {
                GTEST_SKIP() << "no usable GL_TEXTURE_CUBE_MAP_ARRAY on this backend: " << GLErrorName(error);
            }

            const GLuint depthStencil = MakeCubeArrayDepth(GL_DEPTH24_STENCIL8);
            if (const GLenum error = FirstGLError()) {
                GTEST_SKIP() << "no depth-stencil GL_TEXTURE_CUBE_MAP_ARRAY on this backend: "
                             << GLErrorName(error);
            }

            const std::string geometrySource = MakeGeometrySource(kCubeLayerFaces);
            const GLuint program = BuildProgram(geometrySource.c_str(), kLayeredFragmentSource);
            if (program == 0) return;

            glViewport(0, 0, kExtent, kExtent);
            glDisable(GL_SCISSOR_TEST);

            // The probe: its own colour attachment (so the subject texture keeps its poison), the
            // same depth-stencil attachment, and both tests off - so every layer-face must come
            // back painted, whatever the gating below then decides.
            {
                const GLuint probeColor = MakePoisonedCubeArrayColor();
                const GLuint probeFbo = TrackFramebuffer();
                glBindFramebuffer(GL_FRAMEBUFFER, probeFbo);
                glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, probeColor, 0);
                glFramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, depthStencil, 0);
                glDrawBuffer(GL_COLOR_ATTACHMENT0);
                ASSERT_EQ(FirstGLError(), 0u) << "attaching the layered colour + depth-stencil pair failed";
                ASSERT_TRUE(FramebufferIsComplete());
                glDisable(GL_DEPTH_TEST);
                glDisable(GL_STENCIL_TEST);
                DrawLayered(program, /*pass=*/3, /*depth=*/0.0f);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                const std::vector<Rgba8> probeTexels =
                    ReadLevel(GL_TEXTURE_CUBE_MAP_ARRAY, probeColor, kCubeLayerFaces);
                EXPECT_EQ(FirstGLError(), 0u) << "the probe draw or readback errored";
                ExpectEveryLayer(probeTexels, kCubeLayerFaces, /*pass=*/3,
                                 "a layered draw with the depth and stencil tests DISABLED, into a colour + "
                                 "GL_DEPTH_STENCIL_ATTACHMENT cube-map-array pair (all poison means the "
                                 "attachment pair lost the draw outright, which is what a non-layered "
                                 "depth/stencil attachment beside a layered colour one looks like)");
                // The gating assertions below can only add noise once the shape itself is broken.
                if (::testing::Test::HasNonfatalFailure()) return;
            }

            const GLuint fbo = TrackFramebuffer();
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, color, 0);
            glFramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, depthStencil, 0);
            glDrawBuffer(GL_COLOR_ATTACHMENT0);
            ASSERT_EQ(FirstGLError(), 0u) << "attaching the layered colour + depth-stencil pair failed";
            ASSERT_TRUE(FramebufferIsComplete());

            glDepthMask(GL_TRUE);
            glStencilMask(0xFFu);
            glClearDepth(1.0);
            glClearStencil(0);
            glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            ASSERT_EQ(FirstGLError(), 0u) << "clearing the layered depth-stencil attachment errored";

            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LESS);
            glEnable(GL_STENCIL_TEST);
            glStencilFunc(GL_ALWAYS, 1, 0xFFu);
            glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
            DrawLayered(program, /*pass=*/0, /*depth=*/0.0f);

            // Farther than pass 0, so GL_LESS must reject it on every layer.
            glStencilFunc(GL_ALWAYS, 1, 0xFFu);
            glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
            DrawLayered(program, /*pass=*/1, /*depth=*/0.5f);

            // Depth out of the way; only the stencil pass 0 wrote can reject this one.
            glDepthFunc(GL_ALWAYS);
            glStencilFunc(GL_EQUAL, 0, 0xFFu);
            DrawLayered(program, /*pass=*/2, /*depth=*/-0.5f);
            EXPECT_EQ(FirstGLError(), 0u) << "the three layered draws errored";

            glDisable(GL_DEPTH_TEST);
            glDisable(GL_STENCIL_TEST);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            const std::vector<Rgba8> texels = ReadLevel(GL_TEXTURE_CUBE_MAP_ARRAY, color, kCubeLayerFaces);
            EXPECT_EQ(FirstGLError(), 0u) << "reading the cube-map-array level back errored";
            ExpectEveryLayer(texels, kCubeLayerFaces, /*pass=*/0,
                             "layered cube-map-array depth-stencil attachment (a later pass's colour means "
                             "the depth or stencil test did not cover that layer)");

            Gl().EndFrame();
        }

    } // namespace
} // namespace MGITest
