// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/InvalidImageUnitScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - AN IMAGE UNIT NAMES A TEXTURE, BUT NOT A TEXEL THE TEXTURE HAS.
//
// UnboundImageDescriptorScenario's neighbour, one step further in: there the unit holds nothing,
// here it holds a real texture at a (level, layer) that texture does not have. GL 4.6 core 8.26
// makes every such access INVALID, and an invalid access is not an error at draw time: a load
// returns zero in R, G and B, a store has no effect. It is the same observable an empty unit has.
//
// KHR-GL46.shader_image_load_store.incomplete_textures is the conformance shape (case 1 below,
// once as a dispatch and once as the CTS's own fragment-shader draw): a texture with only level 0,
// GL_TEXTURE_MAX_LEVEL 7 and a mipmapping minification filter, bound at level 2. Magma's wire
// descriptor resolve looked the (level, layer) up in the texture's storage, found nothing, and
// died: Fatal{UnmigratedVerb, "Magma:image-view-window"} - a monolith Fail turned into a SIGABRT
// on the Redmi's inproc run (P7 gate 5 dry run). The other cases are the other ways the same
// lookup comes up empty: an immutable texture one level short, an array one slice short, a
// texture that has no storage at all, and a texture view whose own window is shorter than its
// owner's storage.
//
// Every case asserts all three halves of "invalid": the load through the invalid unit reads zero
// (it lands in a COMPLETE destination that started non-zero, so a lost dispatch is red as well),
// the store through the invalid unit changed no texel the invalid texture does have, and no GL
// error was raised.
//
// The two `AStoreThrough...` cases (codex closeout finding 2) store FIRST, through invalid unit A,
// then load through invalid unit B of the same shape and through A: a discarded store must never be
// loaded back. Magma's wire arm answered every invalid storage unit of one (format, target) from
// one shared placeholder image and failed both loads; it now binds a NULL storage descriptor
// (VK_EXT_robustness2 nullDescriptor), or, without one, a placeholder private to the (binding,
// unit) - forced on the host by MGITEST_MAGMA_FORCE_PRIVATE_IMAGE_PLACEHOLDER=1 (the
// `DirectVulkan.{Split,Spawn}.ImageUnitPrivate.` entries), where A's own load is the recorded
// CONTRACT-P7 §12 residual.
//
// Arms. Split/spawn/tcp on both backends. Espryt forwards the unit to the driver, which applies
// 8.26 itself. The MONOLITH DirectVulkan arm is skipped by name: it resolves the same binding to
// "no descriptor" and drops the whole draw (VkTextureManager::GetOrCreateStorageImageView's
// `mipLevel >= resource->mipLevels`), which is why the conformance case is a Fail there - that
// Fail is the P7 gate 5 BASE, and moving it would move the pull build's .text (G1).

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "../Harness/HeadlessGL.h"
#include "../Harness/ScenarioFixture.h"
#include "../Harness/SplitLane.h"
#include "../Harness/SplitRuntimePeek.h"

#ifdef GLAPI
#undef GLAPI
#endif
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glcorearb.h>
#undef GL_GLEXT_PROTOTYPES

namespace MGITest {
    namespace {

        constexpr int kEdge = 8;
        constexpr GLubyte kCompleteDestinationMagic = 0x11;
        constexpr GLubyte kCompleteSourceMagic = 0x22;
        constexpr GLubyte kInvalidDestinationMagic = 0x33;
        constexpr GLubyte kInvalidSourceMagic = 0x44;

        // Units 0..3 and their roles are the conformance case's own.
        constexpr const char* kComputeSource = R"(#version 430 core
layout(local_size_x = 8, local_size_y = 8) in;
layout(rgba8, binding = 0) writeonly uniform image2D u_completeDestination;
layout(rgba8, binding = 1) readonly  uniform image2D u_completeSource;
layout(rgba8, binding = 2) writeonly uniform image2D u_invalidDestination;
layout(rgba8, binding = 3) readonly  uniform image2D u_invalidSource;
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    imageStore(u_completeDestination, p, imageLoad(u_invalidSource, p));
    imageStore(u_invalidDestination, p, imageLoad(u_completeSource, p));
}
)";

        constexpr const char* kVertexSource = R"(#version 430 core
void main() {
    vec2 positions[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
}
)";

        // Codex closeout finding 2: TWO INVALID UNITS MUST NOT ALIAS, AND AN INVALID STORE MUST NOT BE
        // LOADED BACK. Magma's wire arm bound every invalid (and every empty) storage unit of one
        // (format, target) to ONE shared placeholder image, cleared before the pass - so inside the
        // pass a store through invalid unit A was there to be loaded through invalid unit B, and
        // through A itself. 8.26: the store is discarded, both loads return zero. The store comes
        // FIRST here (the four-unit shader above loads its invalid source before it stores to its
        // invalid destination, so it could not see this), then an image barrier, then both loads,
        // each into a complete destination that starts non-zero.
        constexpr GLubyte kStoredThroughInvalid = 0x7F;
        constexpr const char* kAliasComputeSource = R"(#version 430 core
layout(local_size_x = 8, local_size_y = 8) in;
layout(rgba8, binding = 0) writeonly uniform image2D u_loadedThroughB;
layout(rgba8, binding = 1) writeonly uniform image2D u_loadedThroughA;
layout(rgba8, binding = 2) coherent uniform image2D u_invalidA;
layout(rgba8, binding = 3) coherent readonly uniform image2D u_invalidB;
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    imageStore(u_invalidA, p, vec4(127.0 / 255.0));
    memoryBarrierImage();
    imageStore(u_loadedThroughB, p, imageLoad(u_invalidB, p));
    imageStore(u_loadedThroughA, p, imageLoad(u_invalidA, p));
}
)";

        // The conformance case's fragment shader, including its trailing discard.
        constexpr const char* kFragmentSource = R"(#version 430 core
layout(rgba8, binding = 0) writeonly uniform image2D u_completeDestination;
layout(rgba8, binding = 1) readonly  uniform image2D u_completeSource;
layout(rgba8, binding = 2) writeonly uniform image2D u_invalidDestination;
layout(rgba8, binding = 3) readonly  uniform image2D u_invalidSource;
out vec4 o_color;
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 complete = imageLoad(u_completeSource, p);
    vec4 invalid = imageLoad(u_invalidSource, p);
    imageStore(u_completeDestination, p, invalid);
    imageStore(u_invalidDestination, p, complete);
    o_color = vec4(1.0);
    discard;
}
)";

        // Every byte non-zero, and distinct per texture, so "unchanged" and "zero" can never be
        // the same answer.
        std::vector<GLubyte> Pattern(int width, int height, int depth, GLubyte magic) {
            std::vector<GLubyte> texels(static_cast<std::size_t>(width * height * depth) * 4u);
            for (int z = 0; z < depth; ++z) {
                for (int y = 0; y < height; ++y) {
                    for (int x = 0; x < width; ++x) {
                        const std::size_t at = static_cast<std::size_t>(((z * height + y) * width + x) * 4);
                        texels[at + 0] = magic;
                        texels[at + 1] = static_cast<GLubyte>(x + 1);
                        texels[at + 2] = static_cast<GLubyte>(y + 1 + 16 * z);
                        texels[at + 3] = 0xFF;
                    }
                }
            }
            return texels;
        }

        enum class Invalidity {
            MissingMipLevel,      // mutable, level 0 only, MAX_LEVEL 7, bound at level 2 (the CTS shape)
            PastImmutableLevels,  // glTexStorage2D with 2 levels, bound at level 2
            PastArrayLayers,      // 2D array of 2 slices, bound non-layered at layer 2
            NoStorage,            // a named texture that was never given an image
            PastViewWindow,       // a 1-level view of a 3-level owner, bound at view level 1
        };

        class InvalidImageUnitScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                m_wire = SplitRuntimeSkipReason().empty();
                if (Gl().BackendName() == "DirectVulkan" && !m_wire) {
                    GTEST_SKIP() << "the monolith DirectVulkan arm resolves an image unit whose (level, layer) the "
                                    "texture lacks to NO descriptor and drops the whole draw "
                                    "(GetOrCreateStorageImageView: mipLevel >= resource->mipLevels), so the "
                                    "valid half of the work never lands either. That is the P7 gate 5 BASE "
                                    "(KHR-GL46.shader_image_load_store.incomplete_textures Fails there), and "
                                    "changing it moves the pull build's .text (G1). Magma's wire arms and both "
                                    "Espryt arms are the subject.";
                }
                FirstGLError();
            }

            void TearDown() override {
                if (!Ready()) return;
                glUseProgram(0);
                for (GLuint unit = 0; unit < 4; ++unit) {
                    glBindImageTexture(unit, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
                }
                glBindTexture(GL_TEXTURE_2D, 0);
                glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
                if (m_program != 0) glDeleteProgram(m_program);
                if (!m_textures.empty()) {
                    glDeleteTextures(static_cast<GLsizei>(m_textures.size()), m_textures.data());
                }
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                if (m_target.fbo != 0) {
                    BindDefaultFramebuffer();
                    DestroyColorFbo(m_target);
                    glViewport(0, 0, Gl().Width(), Gl().Height());
                }
                FirstGLError();
            }

            static bool LimitIsAtLeast(GLenum limit, GLint wanted) {
                GLint value = 0;
                glGetIntegerv(limit, &value);
                while (glGetError() != GL_NO_ERROR) {
                }
                return value >= wanted;
            }

            GLuint NewTexture() {
                GLuint texture = 0;
                glGenTextures(1, &texture);
                m_textures.push_back(texture);
                return texture;
            }

            GLuint MakeComplete2D(GLubyte magic) {
                const GLuint texture = NewTexture();
                glBindTexture(GL_TEXTURE_2D, texture);
                glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, kEdge, kEdge);
                const auto texels = Pattern(kEdge, kEdge, 1, magic);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kEdge, kEdge, GL_RGBA, GL_UNSIGNED_BYTE, texels.data());
                return texture;
            }

            // The invalid texture for one reason, plus the (level, layer) to bind it at. The
            // levels/layers it DOES have are recorded so the store half can be checked on all of
            // them.
            struct InvalidTexture {
                GLuint texture = 0;
                GLint bindLevel = 0;
                GLint bindLayer = 0;
                GLenum readTarget = GL_TEXTURE_2D;
                GLuint readTexture = 0;           // the texture whose texels are checked (a view's owner)
                std::vector<int> definedLevels;   // levels of readTexture that hold `magic`
                int layers = 1;
                GLubyte magic = 0;
            };

            InvalidTexture MakeInvalid(Invalidity why, GLubyte magic) {
                InvalidTexture made{};
                made.magic = magic;
                switch (why) {
                case Invalidity::MissingMipLevel: {
                    made.texture = NewTexture();
                    glBindTexture(GL_TEXTURE_2D, made.texture);
                    const auto texels = Pattern(kEdge, kEdge, 1, magic);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kEdge, kEdge, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                                 texels.data());
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 7);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR);
                    made.bindLevel = 2;
                    made.definedLevels = {0};
                    break;
                }
                case Invalidity::PastImmutableLevels: {
                    made.texture = NewTexture();
                    glBindTexture(GL_TEXTURE_2D, made.texture);
                    glTexStorage2D(GL_TEXTURE_2D, 2, GL_RGBA8, kEdge, kEdge);
                    for (int level = 0; level < 2; ++level) {
                        const int edge = kEdge >> level;
                        const auto texels = Pattern(edge, edge, 1, magic);
                        glTexSubImage2D(GL_TEXTURE_2D, level, 0, 0, edge, edge, GL_RGBA, GL_UNSIGNED_BYTE,
                                        texels.data());
                    }
                    made.bindLevel = 2;
                    made.definedLevels = {0, 1};
                    break;
                }
                case Invalidity::PastArrayLayers: {
                    made.texture = NewTexture();
                    glBindTexture(GL_TEXTURE_2D_ARRAY, made.texture);
                    glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, kEdge, kEdge, 2);
                    const auto texels = Pattern(kEdge, kEdge, 2, magic);
                    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, kEdge, kEdge, 2, GL_RGBA, GL_UNSIGNED_BYTE,
                                    texels.data());
                    made.bindLayer = 2;
                    made.readTarget = GL_TEXTURE_2D_ARRAY;
                    made.layers = 2;
                    made.definedLevels = {0};
                    break;
                }
                case Invalidity::NoStorage: {
                    made.texture = NewTexture();
                    // Bound once, so the name is an existing texture object glBindImageTexture accepts.
                    glBindTexture(GL_TEXTURE_2D, made.texture);
                    glBindTexture(GL_TEXTURE_2D, 0);
                    break;
                }
                case Invalidity::PastViewWindow: {
                    const GLuint owner = NewTexture();
                    glBindTexture(GL_TEXTURE_2D, owner);
                    glTexStorage2D(GL_TEXTURE_2D, 3, GL_RGBA8, kEdge, kEdge);
                    for (int level = 0; level < 3; ++level) {
                        const int edge = kEdge >> level;
                        const auto texels = Pattern(edge, edge, 1, magic);
                        glTexSubImage2D(GL_TEXTURE_2D, level, 0, 0, edge, edge, GL_RGBA, GL_UNSIGNED_BYTE,
                                        texels.data());
                    }
                    made.texture = NewTexture();
                    // The view's one level is the owner's level 1. View level 1 would be the
                    // owner's level 2, which EXISTS - the window, not the storage, is what the
                    // binding falls outside of.
                    glTextureView(made.texture, GL_TEXTURE_2D, owner, GL_RGBA8, 1, 1, 0, 1);
                    made.bindLevel = 1;
                    made.readTexture = owner;
                    made.definedLevels = {0, 1, 2};
                    break;
                }
                }
                if (made.readTexture == 0) made.readTexture = made.texture;
                glBindTexture(GL_TEXTURE_2D, 0);
                glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
                return made;
            }

            std::vector<GLubyte> ReadLevel(GLenum target, GLuint texture, int level, int layers) {
                const int edge = kEdge >> level;
                std::vector<GLubyte> texels(static_cast<std::size_t>(edge * edge * layers) * 4u, 0xA5);
                glBindTexture(target, texture);
                glGetTexImage(target, level, GL_RGBA, GL_UNSIGNED_BYTE, texels.data());
                glBindTexture(target, 0);
                return texels;
            }

            GLuint BuildCompute(const char* source = kComputeSource) {
                const GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
                glShaderSource(shader, 1, &source, nullptr);
                glCompileShader(shader);
                GLint ok = GL_FALSE;
                glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
                if (ok == GL_FALSE) {
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
                glGetProgramiv(program, GL_LINK_STATUS, &ok);
                if (ok == GL_FALSE) {
                    char log[4096] = {};
                    glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
                    ADD_FAILURE() << "the compute program did not link: " << log;
                    glDeleteProgram(program);
                    return 0;
                }
                return program;
            }

            // The whole case: complete destination/source on units 0/1, the invalid texture on
            // units 2/3 (two textures of the same shape, as in the conformance case), one pass,
            // then the three halves of "invalid".
            void ExpectInvalidAccess(Invalidity why, bool asDraw, const char* what) {
                if (asDraw) {
                    if (!LimitIsAtLeast(GL_MAX_FRAGMENT_IMAGE_UNIFORMS, 4)) {
                        GTEST_SKIP() << "the fragment stage has fewer than four image uniforms";
                    }
                } else if (!LimitIsAtLeast(GL_MAX_COMPUTE_IMAGE_UNIFORMS, 4)) {
                    GTEST_SKIP() << "the compute stage has fewer than four image uniforms";
                }
                const GLuint completeDestination = MakeComplete2D(kCompleteDestinationMagic);
                const GLuint completeSource = MakeComplete2D(kCompleteSourceMagic);
                const InvalidTexture invalidDestination = MakeInvalid(why, kInvalidDestinationMagic);
                const InvalidTexture invalidSource = MakeInvalid(why, kInvalidSourceMagic);
                ASSERT_EQ(FirstGLError(), 0u) << "building the textures raised a GL error (" << what << ")";

                glBindImageTexture(0, completeDestination, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA8);
                glBindImageTexture(1, completeSource, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA8);
                glBindImageTexture(2, invalidDestination.texture, invalidDestination.bindLevel, GL_FALSE,
                                   invalidDestination.bindLayer, GL_READ_WRITE, GL_RGBA8);
                glBindImageTexture(3, invalidSource.texture, invalidSource.bindLevel, GL_FALSE,
                                   invalidSource.bindLayer, GL_READ_WRITE, GL_RGBA8);
                // A binding that is invalid at ACCESS time is still a legal bind (8.26 validates
                // only the unit, the level's sign, the layer's sign, the access and the format).
                ASSERT_EQ(FirstGLError(), 0u) << "glBindImageTexture raised a GL error (" << what << ")";

                if (asDraw) {
                    std::string error;
                    m_program = CompileProgram(kVertexSource, kFragmentSource, &error);
                    ASSERT_NE(m_program, 0u) << error;
                    m_target = MakeColorFbo(kEdge, kEdge);
                    ASSERT_NE(m_target.fbo, 0u) << "could not create the render target";
                    BindFbo(m_target);
                    glGenVertexArrays(1, &m_vao);
                    glBindVertexArray(m_vao);
                    glUseProgram(m_program);
                    glDrawArrays(GL_TRIANGLES, 0, 3);
                    glBindVertexArray(0);
                } else {
                    m_program = BuildCompute();
                    ASSERT_NE(m_program, 0u);
                    glUseProgram(m_program);
                    glDispatchCompute(1, 1, 1);
                }
                glMemoryBarrier(GL_ALL_BARRIER_BITS);
                EXPECT_EQ(FirstGLError(), 0u) << "the pass raised a GL error (" << what << ")";

                // (1) The load through the invalid unit read zero. The complete destination
                // started at 0x11 everywhere, so a pass that never ran is red here too.
                // ALL FOUR CHANNELS, which is stricter than 8.26 (it leaves the A of an invalid
                // load undefined) and exactly what the conformance case compares: Espryt's driver
                // answers (0, 0, 0, 0), and Magma's R32 placeholder used to answer (0, 0, 0, 1) -
                // a green on R, G and B and a Fail on the CTS.
                const auto loaded = ReadLevel(GL_TEXTURE_2D, completeDestination, 0, 1);
                int nonZero = 0;
                int firstX = -1, firstY = -1;
                for (int y = 0; y < kEdge; ++y) {
                    for (int x = 0; x < kEdge; ++x) {
                        const std::size_t at = static_cast<std::size_t>((y * kEdge + x) * 4);
                        if (loaded[at] != 0 || loaded[at + 1] != 0 || loaded[at + 2] != 0 || loaded[at + 3] != 0) {
                            if (nonZero++ == 0) {
                                firstX = x;
                                firstY = y;
                            }
                        }
                    }
                }
                if (nonZero != 0) {
                    const std::size_t at = static_cast<std::size_t>((firstY * kEdge + firstX) * 4);
                    ADD_FAILURE() << nonZero << " of " << kEdge * kEdge << " texels loaded through the invalid "
                                  << what << " unit were not zero; first at (" << firstX << ", " << firstY << ") = ("
                                  << int(loaded[at]) << ", " << int(loaded[at + 1]) << ", " << int(loaded[at + 2])
                                  << ", " << int(loaded[at + 3]) << ") (R = 17 means the pass never ran)";
                }

                // (2) The store through the invalid unit changed nothing the texture has.
                for (const int level : invalidDestination.definedLevels) {
                    const int edge = kEdge >> level;
                    const auto expected = Pattern(edge, edge, invalidDestination.layers, invalidDestination.magic);
                    const auto actual = ReadLevel(invalidDestination.readTarget, invalidDestination.readTexture, level,
                                                  invalidDestination.layers);
                    EXPECT_EQ(actual, expected) << "a store through the invalid " << what
                                                << " unit modified level " << level << " of the texture it names";
                }

                // (3) The valid half of the same pass is intact: the complete source was only read.
                EXPECT_EQ(ReadLevel(GL_TEXTURE_2D, completeSource, 0, 1), Pattern(kEdge, kEdge, 1, kCompleteSourceMagic))
                    << "the complete source changed (" << what << ")";
                EXPECT_EQ(FirstGLError(), 0u) << "the readbacks raised a GL error (" << what << ")";
            }

            // Every texel of `texels` (one kEdge x kEdge RGBA8 level) loaded zero: R, G and B, which
            // is what 8.26 fixes, and A as well where `strictAlpha` (the named-but-missing texel,
            // whose conformance case compares all four; an EMPTY unit's placeholder on the arm
            // without a null descriptor is the numeric-domain R32 one, which expands A to 1 - legal,
            // 8.26 leaves A undefined). `storedAtOrigin` instead expects the stored 0x7F at (0, 0)
            // in R, the one channel every placeholder format carries.
            static void ExpectLoadedZero(const std::vector<GLubyte>& texels, bool storedAtOrigin, bool strictAlpha,
                                         const char* through, const char* what) {
                int wrong = 0;
                int firstX = -1, firstY = -1;
                for (int y = 0; y < kEdge; ++y) {
                    for (int x = 0; x < kEdge; ++x) {
                        const std::size_t at = static_cast<std::size_t>((y * kEdge + x) * 4);
                        const bool ok = storedAtOrigin && x == 0 && y == 0
                                            ? texels[at] == kStoredThroughInvalid
                                            : texels[at] == 0 && texels[at + 1] == 0 && texels[at + 2] == 0 &&
                                                  (!strictAlpha || texels[at + 3] == 0);
                        if (!ok) {
                            if (wrong++ == 0) {
                                firstX = x;
                                firstY = y;
                            }
                        }
                    }
                }
                if (wrong != 0) {
                    const std::size_t at = static_cast<std::size_t>((firstY * kEdge + firstX) * 4);
                    ADD_FAILURE() << wrong << " of " << kEdge * kEdge << " texels loaded through " << through << " ("
                                  << what << ") were not what 8.26 gives; first at (" << firstX << ", " << firstY
                                  << ") = (" << int(texels[at]) << ", " << int(texels[at + 1]) << ", "
                                  << int(texels[at + 2]) << ", " << int(texels[at + 3]) << ") (127 is the value "
                                  << "stored through the invalid unit A, 17/34 means the pass never ran)";
                }
            }

            // Units 2 (A) and 3 (B) are both invalid and of ONE shape - either both name the
            // conformance case's missing mip level of a same-format texture, or both are empty.
            // One dispatch stores 0x7F through A, barriers, and loads through B and through A.
            void ExpectNoAliasThroughInvalidUnits(bool empty, const char* what) {
                if (!LimitIsAtLeast(GL_MAX_COMPUTE_IMAGE_UNIFORMS, 4)) {
                    GTEST_SKIP() << "the compute stage has fewer than four image uniforms";
                }
                // The Magma wire arm without a null storage descriptor (a device lacking
                // VK_EXT_robustness2 nullDescriptor; forced on the host by the knob): each invalid
                // unit gets a placeholder private to its (binding, unit). B's load is still zero -
                // the units no longer alias - but A's load of A's own store in the same pass is
                // not: that is the CONTRACT-P7 §12 residual, and here it is what PROVES the knob
                // reached the server (the null-descriptor arm would read zero there too).
                const bool privatePlaceholderArm = m_wire && Gl().BackendName() == "DirectVulkan" &&
                                                   SplitLane::MarkerIsOne("MGITEST_MAGMA_FORCE_PRIVATE_IMAGE_PLACEHOLDER");
                const GLuint loadedThroughB = MakeComplete2D(kCompleteDestinationMagic);
                const GLuint loadedThroughA = MakeComplete2D(kCompleteSourceMagic);
                InvalidTexture invalidA{};
                InvalidTexture invalidB{};
                if (!empty) {
                    invalidA = MakeInvalid(Invalidity::MissingMipLevel, kInvalidDestinationMagic);
                    invalidB = MakeInvalid(Invalidity::MissingMipLevel, kInvalidSourceMagic);
                }
                ASSERT_EQ(FirstGLError(), 0u) << "building the textures raised a GL error (" << what << ")";

                glBindImageTexture(0, loadedThroughB, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA8);
                glBindImageTexture(1, loadedThroughA, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA8);
                glBindImageTexture(2, invalidA.texture, invalidA.bindLevel, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA8);
                glBindImageTexture(3, invalidB.texture, invalidB.bindLevel, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA8);
                ASSERT_EQ(FirstGLError(), 0u) << "glBindImageTexture raised a GL error (" << what << ")";

                m_program = BuildCompute(kAliasComputeSource);
                ASSERT_NE(m_program, 0u);
                glUseProgram(m_program);
                glDispatchCompute(1, 1, 1);
                glMemoryBarrier(GL_ALL_BARRIER_BITS);
                EXPECT_EQ(FirstGLError(), 0u) << "the pass raised a GL error (" << what << ")";

                ExpectLoadedZero(ReadLevel(GL_TEXTURE_2D, loadedThroughB, 0, 1), false, !empty,
                                 "invalid unit B after a store through invalid unit A of the same shape", what);
                ExpectLoadedZero(ReadLevel(GL_TEXTURE_2D, loadedThroughA, 0, 1), privatePlaceholderArm, !empty,
                                 privatePlaceholderArm
                                     ? "invalid unit A itself on the private-placeholder arm (0x7F at the origin "
                                       "is the recorded same-unit residual AND the proof this arm ran; zero means "
                                       "MGITEST_MAGMA_FORCE_PRIVATE_IMAGE_PLACEHOLDER did not reach the server)"
                                     : "invalid unit A itself after its own store",
                                 what);
                if (!empty) {
                    // The discarded store changed nothing the texture has.
                    for (const InvalidTexture* invalid : {&invalidA, &invalidB}) {
                        EXPECT_EQ(ReadLevel(GL_TEXTURE_2D, invalid->texture, 0, 1),
                                  Pattern(kEdge, kEdge, 1, invalid->magic))
                            << "a store through an invalid unit modified the level its texture has (" << what << ")";
                    }
                }
                EXPECT_EQ(FirstGLError(), 0u) << "the readbacks raised a GL error (" << what << ")";
            }

            bool m_wire = false;
            std::vector<GLuint> m_textures;
            unsigned int m_program = 0;
            GLuint m_vao = 0;
            ColorFbo m_target{};
        };

    } // namespace

    // The conformance shape, as the conformance case runs it: a fragment-shader draw.
    TEST_F(InvalidImageUnitScenario, AMissingMipLevelOfAnIncompleteTextureReadsZeroInADraw) {
        if (!Ready() || IsSkipped()) return;
        ExpectInvalidAccess(Invalidity::MissingMipLevel, true, "missing-mip-level");
    }

    TEST_F(InvalidImageUnitScenario, AMissingMipLevelOfAnIncompleteTextureReadsZeroInADispatch) {
        if (!Ready() || IsSkipped()) return;
        ExpectInvalidAccess(Invalidity::MissingMipLevel, false, "missing-mip-level");
    }

    TEST_F(InvalidImageUnitScenario, ALevelPastAnImmutableTexturesStorageReadsZero) {
        if (!Ready() || IsSkipped()) return;
        ExpectInvalidAccess(Invalidity::PastImmutableLevels, false, "past-immutable-levels");
    }

    TEST_F(InvalidImageUnitScenario, ALayerPastAnArraysLastSliceReadsZero) {
        if (!Ready() || IsSkipped()) return;
        ExpectInvalidAccess(Invalidity::PastArrayLayers, false, "past-array-layers");
    }

    TEST_F(InvalidImageUnitScenario, ATextureWithNoStorageReadsZero) {
        if (!Ready() || IsSkipped()) return;
        ExpectInvalidAccess(Invalidity::NoStorage, false, "no-storage");
    }

    TEST_F(InvalidImageUnitScenario, ALevelPastATextureViewsWindowReadsZero) {
        if (!Ready() || IsSkipped()) return;
        if (Gl().BackendName() == "DirectGLES") {
            const char* enabled = std::getenv("MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW");
            if (enabled == nullptr || std::string(enabled) != "1") {
                GTEST_SKIP() << "Espryt withholds glTextureView unless MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW=1";
            }
        }
        ExpectInvalidAccess(Invalidity::PastViewWindow, false, "past-view-window");
    }

    // Codex closeout finding 2: a store through invalid unit A, an image barrier, then loads
    // through invalid unit B of the same (format, target) and through A - both zero.
    TEST_F(InvalidImageUnitScenario, AStoreThroughAnInvalidUnitIsLoadedThroughNeitherAnotherInvalidUnitNorItself) {
        if (!Ready() || IsSkipped()) return;
        ExpectNoAliasThroughInvalidUnits(false, "missing-mip-level units");
    }

    // The same through two EMPTY units (glBindImageTexture(unit, 0, ...)): 8.26 makes those
    // accesses invalid too, and Magma's wire arm answered them from the same shared placeholder.
    TEST_F(InvalidImageUnitScenario, AStoreThroughAnEmptyUnitIsLoadedThroughNeitherAnotherEmptyUnitNorItself) {
        if (!Ready() || IsSkipped()) return;
        ExpectNoAliasThroughInvalidUnits(true, "empty units");
    }

} // namespace MGITest
