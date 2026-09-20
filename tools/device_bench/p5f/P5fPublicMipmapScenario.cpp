// Device-only P5f preparation. Public GL calls, no MobileGL internal symbols.
#include "Harness/ScenarioFixture.h"
#include <array>
#ifdef GLAPI
#undef GLAPI
#endif
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glcorearb.h>
#undef GL_GLEXT_PROTOTYPES

namespace MGITest {
class P5fPublicMipmapScenario : public ScenarioTest {
protected:
    void Check(bool gpuWritten) {
        if (!Ready()) return;
        GLuint texture = 0, fbo = 0;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        std::array<GLubyte, 4 * 4 * 4> red{};
        for (std::size_t i = 0; i < red.size(); i += 4) {
            red[i] = 255;
            red[i + 3] = 255;
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, red.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 2);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        if (gpuWritten) {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
            ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum{GL_FRAMEBUFFER_COMPLETE});
            glDisable(GL_SCISSOR_TEST);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        glGenerateMipmap(GL_TEXTURE_2D);
        for (GLint level = 0; level <= 2; ++level) {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, level);
            ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum{GL_FRAMEBUFFER_COMPLETE}) << level;
            std::array<GLubyte, 4> pixel{71, 72, 73, 74};
            glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
            EXPECT_EQ(pixel[0], gpuWritten ? 0 : 255) << "mip=" << level;
            EXPECT_EQ(pixel[1], gpuWritten ? 255 : 0) << "mip=" << level;
            EXPECT_EQ(pixel[2], 0) << "mip=" << level;
            EXPECT_EQ(pixel[3], 255) << "mip=" << level;
        }
        EXPECT_EQ(glGetError(), GLenum{GL_NO_ERROR});
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &texture);
        Gl().EndFrame();
    }
};

TEST_F(P5fPublicMipmapScenario, UploadedRgba8BaseGeneratesRedMipChain) { Check(false); }
TEST_F(P5fPublicMipmapScenario, GpuWrittenBaseGeneratesGreenMipChainWithoutStaleCpuUpload) { Check(true); }
} // namespace MGITest
