// Wire view/framebuffer caches must preserve public GL state transitions. All
// draws in each case precede its first readback: no Finish/Present can retire an
// old view between A -> B -> A or hide an allocation change from an in-flight draw.
#include "../Harness/ScenarioFixture.h"
#include "../Harness/SplitRuntimePeek.h"
#include <array>
#include <vector>
#ifdef GLAPI
#undef GLAPI
#endif
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glcorearb.h>
#undef GL_GLEXT_PROTOTYPES

namespace MGITest {
namespace {
using Pixel = std::array<GLubyte, 4>;
constexpr Pixel red{255, 0, 0, 255}, green{0, 255, 0, 255}, blue{0, 0, 255, 255};
constexpr Pixel yellow{255, 255, 0, 255}, black{0, 0, 0, 255};
constexpr int kWidth = 32, kHeight = 8;
constexpr const char* kVertex = R"(#version 430 core
void main() {
    vec2 p[3] = vec2[3](vec2(-1,-1), vec2(3,-1), vec2(-1,3));
    gl_Position = vec4(p[gl_VertexID], 0, 1);
})";
constexpr const char* kSample2D = R"(#version 430 core
uniform sampler2D src;
layout(location=0) out vec4 color;
void main() { color = texelFetch(src, ivec2(0), 0); })";
constexpr const char* kSampleArray = R"(#version 430 core
uniform sampler2DArray src;
layout(location=0) out vec4 color;
void main() { color = texelFetch(src, ivec3(0,0,1), 0); })";
constexpr const char* kTint = R"(#version 430 core
uniform vec4 tint;
layout(location=0) out vec4 color;
void main() { color = tint; })";

class MagmaWireCacheScenario : public ScenarioTest {
protected:
    GLuint target = 0;
    std::vector<GLuint> textures, fbos, programs, vaos;

    void SetUp() override {
        ScenarioTest::SetUp();
        if (!Ready()) return;
        if (!SplitLane::MarkerIsOne("MGITEST_MAGMA_CACHES_LANE"))
            GTEST_SKIP() << "dedicated Magma cache lane only";
        ASSERT_EQ(Gl().BackendName(), "DirectVulkan");
        const auto runtime = PeekSplitRuntime();
        ASSERT_TRUE(runtime.peekAvailable && runtime.sessionActive && runtime.transportResolved);
        ASSERT_EQ(runtime.transportName, "inproc");
        ASSERT_TRUE(SplitLane::MarkerIsOne("MOBILEGL_IPC_ROLE_SPLIT_STATE"));
        ASSERT_TRUE(SplitLane::MarkerIsOne("MOBILEGL_IPC_STRICT_ERRORS"));
        glActiveTexture(GL_TEXTURE0);
        glBindSampler(0, 0);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glDisable(GL_RASTERIZER_DISCARD);
        glDisable(GL_FRAMEBUFFER_SRGB);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        GLuint vao = 0;
        glGenVertexArrays(1, &vao);
        vaos.push_back(vao);
        glBindVertexArray(vao);
        target = NewFramebuffer(NewTexture(kWidth, kHeight, black));
    }

    void TearDown() override {
        if (!Ready()) return;
        glDisable(GL_FRAMEBUFFER_SRGB);
        glUseProgram(0);
        glBindVertexArray(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (!vaos.empty()) glDeleteVertexArrays(GLsizei(vaos.size()), vaos.data());
        if (!fbos.empty()) glDeleteFramebuffers(GLsizei(fbos.size()), fbos.data());
        if (!textures.empty()) glDeleteTextures(GLsizei(textures.size()), textures.data());
        for (GLuint program : programs) glDeleteProgram(program);
    }

    static std::vector<Pixel> Pixels(int width, int height, Pixel color) {
        return std::vector<Pixel>(size_t(width) * size_t(height), color);
    }

    GLuint TextureName() {
        GLuint texture = 0;
        glGenTextures(1, &texture);
        textures.push_back(texture);
        return texture;
    }

    void DefineTexture(GLuint texture, int width, int height, Pixel color, GLenum format = GL_RGBA8) {
        const auto pixels = Pixels(width, height, color);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    }

    static void Nearest(GLenum target) {
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }

    GLuint NewTexture(int width, int height, Pixel color, GLenum format = GL_RGBA8) {
        const GLuint texture = TextureName();
        DefineTexture(texture, width, height, color, format);
        Nearest(GL_TEXTURE_2D);
        return texture;
    }

    GLuint NewArray() {
        const GLuint texture = TextureName();
        glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
        glTexStorage3D(GL_TEXTURE_2D_ARRAY, 2, GL_RGBA8, 8, 8, 2);
        Nearest(GL_TEXTURE_2D_ARRAY);
        for (int level = 0; level < 2; ++level) {
            const int size = 8 >> level;
            const auto pixels = Pixels(size, size, black);
            for (int layer = 0; layer < 2; ++layer)
                glTexSubImage3D(GL_TEXTURE_2D_ARRAY, level, 0, 0, layer, size, size, 1,
                                GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        }
        return texture;
    }

    GLuint NewFramebuffer(GLuint texture = 0) {
        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        fbos.push_back(fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        if (texture) {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
            EXPECT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE));
        }
        return fbo;
    }

    GLuint Link(const char* fragment) {
        const GLuint program = glCreateProgram();
        programs.push_back(program);
        const std::array<GLenum, 2> kinds{GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
        const std::array<const char*, 2> sources{kVertex, fragment};
        for (size_t i = 0; i < kinds.size(); ++i) {
            const GLuint shader = glCreateShader(kinds[i]);
            glShaderSource(shader, 1, &sources[i], nullptr);
            glCompileShader(shader);
            GLint ok = GL_FALSE;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
            std::array<GLchar, 2048> log{};
            if (!ok) glGetShaderInfoLog(shader, GLsizei(log.size()), nullptr, log.data());
            EXPECT_EQ(ok, GL_TRUE) << log.data();
            glAttachShader(program, shader);
            glDeleteShader(shader);
        }
        glLinkProgram(program);
        GLint ok = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        std::array<GLchar, 2048> log{};
        if (!ok) glGetProgramInfoLog(program, GLsizei(log.size()), nullptr, log.data());
        EXPECT_EQ(ok, GL_TRUE) << log.data();
        return ok ? program : 0;
    }

    static void DrawTile(int tile) {
        glViewport(tile * 8, 0, 8, kHeight);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    static void Tint(GLint location, Pixel color) {
        glUniform4f(location, color[0] / 255.0f, color[1] / 255.0f,
                    color[2] / 255.0f, color[3] / 255.0f);
    }

    void ExpectPixel(int x, int y, Pixel expected, const char* why, int tolerance = 0) {
        Pixel actual{};
        glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, actual.data());
        for (size_t channel = 0; channel < actual.size(); ++channel)
            EXPECT_NEAR(int(actual[channel]), int(expected[channel]), tolerance) << why << " channel=" << channel;
        EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << why;
    }
};

TEST_F(MagmaWireCacheScenario, SamplerSwizzleAtoBtoAKeepsEachQueuedPixel) {
    if (!Ready() || IsSkipped()) return;
    const GLuint program = Link(kSample2D);
    ASSERT_NE(program, 0u);
    NewTexture(2, 2, red);
    glUseProgram(program);
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    const GLint identity[]{GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA};
    const GLint swapped[]{GL_GREEN, GL_RED, GL_BLUE, GL_ALPHA};
    glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, identity);
    DrawTile(0);
    glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swapped);
    DrawTile(1);
    glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, identity);
    DrawTile(2);
    DrawTile(3); // The unchanged view is eligible for actual cache reuse too.
    ExpectPixel(4, 4, red, "swizzle A before B");
    ExpectPixel(12, 4, green, "swizzle B has its own descriptor");
    ExpectPixel(20, 4, red, "swizzle A restored");
    ExpectPixel(28, 4, red, "unchanged A reused");
}

TEST_F(MagmaWireCacheScenario, SamplerRespecifyKeepsOldAndNewNativeImages) {
    if (!Ready() || IsSkipped()) return;
    const GLuint program = Link(kSample2D);
    ASSERT_NE(program, 0u);
    const GLuint source = NewTexture(2, 2, red);
    glUseProgram(program);
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    DrawTile(0);
    // TexImage may require a wire reply; it must still preserve the old GPU
    // image referenced by the first draw. There is no intervening GPU readback.
    DefineTexture(source, 4, 4, green);
    DrawTile(1);
    DefineTexture(source, 2, 2, blue);
    DrawTile(2);
    ExpectPixel(4, 4, red, "old allocation before first respecify");
    ExpectPixel(12, 4, green, "larger replacement allocation");
    ExpectPixel(20, 4, blue, "original dimensions with a new allocation");
}

TEST_F(MagmaWireCacheScenario, SamplerViewsDistinguishMipLayerAndViewType) {
    if (!Ready() || IsSkipped()) return;
    const GLuint sample2D = Link(kSample2D), sampleArray = Link(kSampleArray);
    ASSERT_NE(sample2D, 0u);
    ASSERT_NE(sampleArray, 0u);
    const GLuint root = NewArray();
    const auto upload = [&](int level, int layer, Pixel color) {
        const int size = 8 >> level;
        const auto pixels = Pixels(size, size, color);
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, level, 0, 0, layer, size, size, 1,
                        GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    };
    upload(0, 0, red);
    upload(0, 1, green);
    upload(1, 1, yellow);
    const GLuint viewA = TextureName(), viewB = TextureName();
    glTextureView(viewA, GL_TEXTURE_2D, root, GL_RGBA8, 0, 1, 0, 1);
    glTextureView(viewB, GL_TEXTURE_2D, root, GL_RGBA8, 1, 1, 1, 1);
    for (GLuint view : {viewA, viewB}) {
        glBindTexture(GL_TEXTURE_2D, view);
        Nearest(GL_TEXTURE_2D);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glUseProgram(sample2D);
    glBindTexture(GL_TEXTURE_2D, viewA);
    DrawTile(0);
    glBindTexture(GL_TEXTURE_2D, viewB);
    DrawTile(1);
    glBindTexture(GL_TEXTURE_2D, viewA);
    DrawTile(2);
    glUseProgram(sampleArray);
    glBindTexture(GL_TEXTURE_2D_ARRAY, root);
    DrawTile(3);
    ExpectPixel(4, 4, red, "2D view of root mip 0 layer 0");
    ExpectPixel(12, 4, yellow, "2D view of root mip 1 layer 1");
    ExpectPixel(20, 4, red, "first subresource view restored");
    ExpectPixel(28, 4, green, "array view type retains layer selection");
}

TEST_F(MagmaWireCacheScenario, FramebuffersAtoBtoAKeepDistinctAttachments) {
    if (!Ready() || IsSkipped()) return;
    const GLuint program = Link(kTint);
    ASSERT_NE(program, 0u);
    const GLint tint = glGetUniformLocation(program, "tint");
    ASSERT_GE(tint, 0);
    const GLuint a = NewFramebuffer(NewTexture(kWidth, kHeight, black));
    const GLuint b = NewFramebuffer(NewTexture(kWidth, kHeight, black));
    glUseProgram(program);
    glBindFramebuffer(GL_FRAMEBUFFER, a); Tint(tint, red); DrawTile(0);
    glBindFramebuffer(GL_FRAMEBUFFER, b); Tint(tint, green); DrawTile(1);
    glBindFramebuffer(GL_FRAMEBUFFER, a); Tint(tint, blue); DrawTile(2);
    ExpectPixel(4, 4, red, "A retains its first draw");
    ExpectPixel(12, 4, black, "B draw did not use A's compatible framebuffer");
    ExpectPixel(20, 4, blue, "A selected again after B");
    glBindFramebuffer(GL_FRAMEBUFFER, b);
    ExpectPixel(4, 4, black, "first A draw did not reach B");
    ExpectPixel(12, 4, green, "B owns its draw");
    ExpectPixel(20, 4, black, "last A draw did not reach B");
}

TEST_F(MagmaWireCacheScenario, FramebufferReattachmentDistinguishesMipAndLayer) {
    if (!Ready() || IsSkipped()) return;
    const GLuint program = Link(kTint);
    ASSERT_NE(program, 0u);
    const GLint tint = glGetUniformLocation(program, "tint");
    ASSERT_GE(tint, 0);
    const GLuint root = NewArray();
    NewFramebuffer();
    const auto attach = [&](int level, int layer) {
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, root, level, layer);
    };
    glUseProgram(program);
    attach(0, 0); glViewport(0, 0, 8, 8); Tint(tint, red); glDrawArrays(GL_TRIANGLES, 0, 3);
    attach(1, 1); glViewport(0, 0, 4, 4); Tint(tint, green); glDrawArrays(GL_TRIANGLES, 0, 3);
    attach(0, 1); glViewport(0, 0, 8, 8); Tint(tint, blue); glDrawArrays(GL_TRIANGLES, 0, 3);
    attach(0, 0); glViewport(0, 0, 4, 8); Tint(tint, yellow); glDrawArrays(GL_TRIANGLES, 0, 3);
    ExpectPixel(2, 4, yellow, "restored mip 0 layer 0 left half");
    ExpectPixel(6, 4, red, "restored mip 0 layer 0 preserves first draw");
    attach(1, 1); ExpectPixel(2, 2, green, "smaller mip 1 layer 1 attachment");
    attach(0, 1); ExpectPixel(6, 4, blue, "same mip with a different layer");
    attach(1, 0); ExpectPixel(2, 2, black, "untouched mip 1 layer 0");
}

TEST_F(MagmaWireCacheScenario, FramebufferSrgbPolicyAtoBtoAUsesDistinctFormats) {
    if (!Ready() || IsSkipped()) return;
    const GLuint program = Link(kTint);
    ASSERT_NE(program, 0u);
    const GLint tint = glGetUniformLocation(program, "tint");
    ASSERT_GE(tint, 0);
    NewFramebuffer(NewTexture(kWidth, kHeight, black, GL_SRGB8_ALPHA8));
    glUseProgram(program);
    glUniform4f(tint, 0.5f, 0.5f, 0.5f, 1.0f);
    DrawTile(0);
    glEnable(GL_FRAMEBUFFER_SRGB); DrawTile(1);
    glDisable(GL_FRAMEBUFFER_SRGB); DrawTile(2);
    ExpectPixel(4, 4, Pixel{128, 128, 128, 255}, "linear writes before conversion", 1);
    ExpectPixel(12, 4, Pixel{188, 188, 188, 255}, "sRGB attachment write conversion", 1);
    ExpectPixel(20, 4, Pixel{128, 128, 128, 255}, "linear attachment format restored", 1);
}

TEST_F(MagmaWireCacheScenario, FramebufferRespecifyRebindsTheNewNativeImage) {
    if (!Ready() || IsSkipped()) return;
    const GLuint paint = Link(kTint), sample = Link(kSample2D);
    ASSERT_NE(paint, 0u);
    ASSERT_NE(sample, 0u);
    const GLint tint = glGetUniformLocation(paint, "tint");
    ASSERT_GE(tint, 0);
    const GLuint source = NewTexture(8, 8, black), sourceFbo = NewFramebuffer(source);
    const auto drawThenSample = [&](Pixel color, int width, int tile) {
        glBindFramebuffer(GL_FRAMEBUFFER, sourceFbo);
        glUseProgram(paint); Tint(tint, color);
        glViewport(0, 0, width, 8); glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindFramebuffer(GL_FRAMEBUFFER, target);
        glUseProgram(sample); glBindTexture(GL_TEXTURE_2D, source); DrawTile(tile);
    };
    drawThenSample(red, 8, 0);
    DefineTexture(source, 16, 8, black);
    drawThenSample(green, 16, 1);
    DefineTexture(source, 8, 8, black);
    drawThenSample(blue, 8, 2);
    ExpectPixel(4, 4, red, "pixels produced by the original attachment allocation");
    ExpectPixel(12, 4, green, "same FBO writes the larger replacement allocation");
    ExpectPixel(20, 4, blue, "same dimensions cannot revive the retired attachment");
}
} // namespace
} // namespace MGITest
