// MobileGL - MobileGL/MG_Test/Framebuffer/FramebufferTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <gtest/gtest.h>

#include "Includes.h"
#include "Init.h"
#include <MG_Backend/BackendObjects.h>
#include <MG_Impl/GLImpl/Buffer/GL_Buffer.h>
#include <MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.h>
#include <MG_Impl/GLImpl/Getter/GL_Getter.h>
#include <MG_Impl/GLImpl/Texture/GL_Texture.h>
#include <MG_State/GLState/Core.h>

using namespace MobileGL;

namespace {
    SharedPtr<MG_State::GLState::FramebufferObject> g_lastBlitReadFramebuffer;
    SharedPtr<MG_State::GLState::FramebufferObject> g_lastBlitDrawFramebuffer;
    Int g_blitNamedFramebufferCallCount = 0;
    SharedPtr<MG_State::GLState::FramebufferObject> g_lastClearFramebuffer;
    GLenum g_lastClearBuffer = GL_NONE;
    GLint g_lastClearDrawbuffer = -1;
    GLfloat g_lastClearDepth = -1.0f;
    GLint g_lastClearStencil = -1;
    FloatVec4 g_lastClearColor = {};
    Int g_clearNamedFramebufferfvCallCount = 0;
    Int g_clearNamedFramebufferfiCallCount = 0;
    Int g_readPixelsCallCount = 0;
    GLenum g_lastReadPixelsFormat = GL_NONE;
    GLenum g_lastReadPixelsType = GL_NONE;

    void RecordBlitNamedFramebuffer(const SharedPtr<MG_State::GLState::FramebufferObject>& readFramebuffer,
                                    const SharedPtr<MG_State::GLState::FramebufferObject>& drawFramebuffer,
                                    GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum) {
        g_lastBlitReadFramebuffer = readFramebuffer;
        g_lastBlitDrawFramebuffer = drawFramebuffer;
        ++g_blitNamedFramebufferCallCount;
    }

    void RecordClearNamedFramebufferfv(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer,
                                       GLenum buffer, GLint drawbuffer, const GLfloat* value) {
        g_lastClearFramebuffer = framebuffer;
        g_lastClearBuffer = buffer;
        g_lastClearDrawbuffer = drawbuffer;
        if (value) {
            if (buffer == GL_COLOR) {
                g_lastClearColor = FloatVec4(value[0], value[1], value[2], value[3]);
            } else if (buffer == GL_DEPTH) {
                g_lastClearDepth = value[0];
            }
        }
        ++g_clearNamedFramebufferfvCallCount;
    }

    void RecordClearNamedFramebufferfi(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer,
                                       GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil) {
        g_lastClearFramebuffer = framebuffer;
        g_lastClearBuffer = buffer;
        g_lastClearDrawbuffer = drawbuffer;
        g_lastClearDepth = depth;
        g_lastClearStencil = stencil;
        ++g_clearNamedFramebufferfiCallCount;
    }

    void RecordReadPixels(GLint, GLint, GLsizei, GLsizei, GLenum format, GLenum type, void*) {
        ++g_readPixelsCallCount;
        g_lastReadPixelsFormat = format;
        g_lastReadPixelsType = type;
    }
} // namespace

class FramebufferTest : public ::testing::Test {
protected:
    void SetUp() override {
        MobileGL::Initialize();
        const auto defaultFramebuffer = MG_State::pGLContext->GetFramebufferObject(0);
        ASSERT_NE(defaultFramebuffer, nullptr);
        MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).Bind(defaultFramebuffer);
        MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read).Bind(defaultFramebuffer);
        defaultFramebuffer->SetDrawBuffer(0, FramebufferAttachmentType::BackLeft);
        for (Uint i = 1; i < MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS; ++i) {
            defaultFramebuffer->SetDrawBuffer(i, FramebufferAttachmentType::None);
        }
        defaultFramebuffer->SetReadBuffer(FramebufferAttachmentType::BackLeft);

        g_lastBlitReadFramebuffer = nullptr;
        g_lastBlitDrawFramebuffer = nullptr;
        g_blitNamedFramebufferCallCount = 0;
        g_lastClearFramebuffer = nullptr;
        g_lastClearBuffer = GL_NONE;
        g_lastClearDrawbuffer = -1;
        g_lastClearDepth = -1.0f;
        g_lastClearStencil = -1;
        g_lastClearColor = {};
        g_clearNamedFramebufferfvCallCount = 0;
        g_clearNamedFramebufferfiCallCount = 0;
        g_readPixelsCallCount = 0;
        g_lastReadPixelsFormat = GL_NONE;
        g_lastReadPixelsType = GL_NONE;
        MG_Backend::gBackendFunctionsTable.GL.BlitNamedFramebuffer = nullptr;
        MG_Backend::gBackendFunctionsTable.GL.ClearNamedFramebufferfv = nullptr;
        MG_Backend::gBackendFunctionsTable.GL.ClearNamedFramebufferfi = nullptr;
        MG_Backend::gBackendFunctionsTable.GL.ReadPixels = nullptr;
    }
};

TEST_F(FramebufferTest, CreateFramebuffersCreatesObjectsImmediately) {
    GLuint framebuffers[2] = {};
    MG_Impl::GLImpl::CreateFramebuffers(2, framebuffers);

    EXPECT_NE(framebuffers[0], 0u);
    EXPECT_NE(framebuffers[1], 0u);
    EXPECT_TRUE(MG_State::pGLContext->ValidateFramebufferObject(framebuffers[0]));
    EXPECT_TRUE(MG_State::pGLContext->ValidateFramebufferObject(framebuffers[1]));
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(FramebufferTest, DefaultFramebufferIdentityTracksFramebufferNameZero) {
    const auto defaultFramebuffer = MG_State::pGLContext->GetFramebufferObject(0);
    ASSERT_NE(defaultFramebuffer, nullptr);
    EXPECT_TRUE(defaultFramebuffer->IsDefaultFramebuffer());
    const auto defaultFramebufferCopy = *defaultFramebuffer;
    EXPECT_TRUE(defaultFramebufferCopy.IsDefaultFramebuffer());

    GLuint framebuffer = 0;
    MG_Impl::GLImpl::CreateFramebuffers(1, &framebuffer);
    const auto userFramebuffer = MG_State::pGLContext->GetFramebufferObject(framebuffer);
    ASSERT_NE(userFramebuffer, nullptr);
    EXPECT_FALSE(userFramebuffer->IsDefaultFramebuffer());
}

TEST_F(FramebufferTest, NamedFramebufferTextureAttachesWithoutChangingBindings) {
    GLuint framebuffer = 0;
    MG_Impl::GLImpl::CreateFramebuffers(1, &framebuffer);

    Vector<Uint> textureNames;
    MG_State::pGLContext->GenTextureNames(1, textureNames);
    MG_State::pGLContext->CreateTextureObject(textureNames[0], TextureTarget::Texture2D);

    const auto originalDraw =
        MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject();
    const auto originalRead =
        MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read).GetBoundObject();

    MG_Impl::GLImpl::NamedFramebufferTexture(framebuffer, GL_COLOR_ATTACHMENT0, textureNames[0], 3);

    const auto framebufferObject = MG_State::pGLContext->GetFramebufferObject(framebuffer);
    const auto& attachment = framebufferObject->GetAttachment(FramebufferAttachmentType::Color0);
    EXPECT_TRUE(attachment.IsTexture());
    EXPECT_EQ(attachment.GetTexture()->GetExternalIndex(), textureNames[0]);
    EXPECT_EQ(attachment.GetTextureLevel(), 3);
    EXPECT_EQ(MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject(), originalDraw);
    EXPECT_EQ(MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read).GetBoundObject(), originalRead);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(FramebufferTest, NamedDepthFramebufferTextureStorageIsCompleteWithoutBinding) {
    GLuint framebuffer = 0;
    GLuint texture = 0;
    MG_Impl::GLImpl::CreateFramebuffers(1, &framebuffer);
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &texture);

    const auto originalDraw =
        MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject();
    const auto originalRead =
        MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read).GetBoundObject();

    MG_Impl::GLImpl::TextureStorage2D(texture, 1, GL_DEPTH_COMPONENT24, 64, 32);
    MG_Impl::GLImpl::NamedFramebufferTexture(framebuffer, GL_DEPTH_ATTACHMENT, texture, 0);

    EXPECT_EQ(MG_Impl::GLImpl::CheckNamedFramebufferStatus(framebuffer, GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);
    EXPECT_EQ(MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject(), originalDraw);
    EXPECT_EQ(MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read).GetBoundObject(), originalRead);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(FramebufferTest, FramebufferTextureBumpsAttachmentVersionOnlyOnce) {
    GLuint framebuffer = 0;
    GLuint texture = 0;
    MG_Impl::GLImpl::CreateFramebuffers(1, &framebuffer);
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &texture);
    MG_Impl::GLImpl::TextureStorage2D(texture, 1, GL_RGBA8, 64, 32);
    MG_Impl::GLImpl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);

    const auto framebufferObject = MG_State::pGLContext->GetFramebufferObject(framebuffer);
    const auto beforeVersions = framebufferObject->GetAllFramebufferAttachmentVersions();
    const auto beforeObjectVersion = framebufferObject->GetObjectVersion();

    MG_Impl::GLImpl::FramebufferTexture(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, texture, 0);

    const auto afterVersions = framebufferObject->GetAllFramebufferAttachmentVersions();
    EXPECT_EQ(afterVersions[static_cast<SizeT>(FramebufferAttachmentType::Color0)],
              beforeVersions[static_cast<SizeT>(FramebufferAttachmentType::Color0)] + 1);
    EXPECT_EQ(framebufferObject->GetObjectVersion(), beforeObjectVersion + 1);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(FramebufferTest, ReadPixelsAllowsPersistentMappedPixelPackBuffer) {
    GLuint framebuffer = 0;
    GLuint texture = 0;
    GLuint pixelPackBuffer = 0;
    MG_Impl::GLImpl::CreateFramebuffers(1, &framebuffer);
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &texture);
    MG_Impl::GLImpl::CreateBuffers(1, &pixelPackBuffer);

    MG_Impl::GLImpl::TextureStorage2D(texture, 1, GL_RGBA8, 4, 4);
    MG_Impl::GLImpl::NamedFramebufferTexture(framebuffer, GL_COLOR_ATTACHMENT0, texture, 0);
    MG_Impl::GLImpl::BindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);

    MG_Impl::GLImpl::BindBuffer(GL_PIXEL_PACK_BUFFER, pixelPackBuffer);
    MG_Impl::GLImpl::BufferStorage(GL_PIXEL_PACK_BUFFER, 4 * 4 * 4, nullptr,
                                   GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT);
    ASSERT_NE(MG_Impl::GLImpl::MapBufferRange(GL_PIXEL_PACK_BUFFER, 0, 4 * 4 * 4,
                                              GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT),
              nullptr);

    MG_Backend::gBackendFunctionsTable.GL.ReadPixels = RecordReadPixels;
    MG_Impl::GLImpl::ReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    EXPECT_EQ(g_readPixelsCallCount, 1);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(FramebufferTest, ReadPixelsRejectsMismatchedPackedTypeFormatPairs) {
    GLuint framebuffer = 0;
    GLuint texture = 0;
    MG_Impl::GLImpl::CreateFramebuffers(1, &framebuffer);
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &texture);
    MG_Impl::GLImpl::TextureStorage2D(texture, 1, GL_RGBA8, 4, 4);
    MG_Impl::GLImpl::NamedFramebufferTexture(framebuffer, GL_COLOR_ATTACHMENT0, texture, 0);
    MG_Impl::GLImpl::BindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);

    MG_Backend::gBackendFunctionsTable.GL.ReadPixels = RecordReadPixels;
    Uint8 pixelStorage[4 * 4 * 4] = {};

    // Packed RGB type with a non-RGB format must never reach the backend (GL CTS packed_pixels
    // reads GL_RED with GL_UNSIGNED_SHORT_5_6_5 and expects an error).
    MG_Impl::GLImpl::ReadPixels(0, 0, 4, 4, GL_RED, GL_UNSIGNED_SHORT_5_6_5, pixelStorage);
    EXPECT_EQ(g_readPixelsCallCount, 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_OPERATION);

    // Packed RGBA type with a non-RGBA/BGRA format is rejected as well.
    MG_Impl::GLImpl::ReadPixels(0, 0, 4, 4, GL_RGB, GL_UNSIGNED_INT_8_8_8_8, pixelStorage);
    EXPECT_EQ(g_readPixelsCallCount, 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_OPERATION);

    // Packed depth-stencil type requires the DEPTH_STENCIL format.
    MG_Impl::GLImpl::ReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_INT_24_8, pixelStorage);
    EXPECT_EQ(g_readPixelsCallCount, 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_OPERATION);

    // A plain RGBA/UNSIGNED_BYTE readback keeps working.
    MG_Impl::GLImpl::ReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, pixelStorage);
    EXPECT_EQ(g_readPixelsCallCount, 1);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(FramebufferTest, ReadPixelsForwardsSingleChannelDesktopClientFormats) {
    GLuint framebuffer = 0;
    GLuint texture = 0;
    MG_Impl::GLImpl::CreateFramebuffers(1, &framebuffer);
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &texture);
    MG_Impl::GLImpl::TextureStorage2D(texture, 1, GL_RGBA8, 4, 4);
    MG_Impl::GLImpl::NamedFramebufferTexture(framebuffer, GL_COLOR_ATTACHMENT0, texture, 0);
    MG_Impl::GLImpl::BindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);

    MG_Backend::gBackendFunctionsTable.GL.ReadPixels = RecordReadPixels;
    Uint8 pixelStorage[4 * 4 * 4] = {};

    // Desktop GL treats GL_GREEN/GL_BLUE/GL_ALPHA as valid ReadPixels client formats (GL CTS
    // packed_pixels rgba8_format_green failed with GL_INVALID_ENUM before). The state layer must
    // validate them and forward the raw enum to the backend, which extracts the source channel
    // from a wide RGBA read.
    const GLenum singleChannelFormats[] = {GL_GREEN, GL_BLUE, GL_ALPHA};
    Int expectedCallCount = 0;
    for (const GLenum format : singleChannelFormats) {
        MG_Impl::GLImpl::ReadPixels(0, 0, 4, 4, format, GL_UNSIGNED_BYTE, pixelStorage);
        EXPECT_EQ(g_readPixelsCallCount, ++expectedCallCount);
        EXPECT_EQ(g_lastReadPixelsFormat, format);
        EXPECT_EQ(g_lastReadPixelsType, static_cast<GLenum>(GL_UNSIGNED_BYTE));
        EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    }

    // Packed-type pairing still applies: packed RGB/RGBA types never pair with single-channel formats.
    MG_Impl::GLImpl::ReadPixels(0, 0, 4, 4, GL_GREEN, GL_UNSIGNED_SHORT_5_6_5, pixelStorage);
    EXPECT_EQ(g_readPixelsCallCount, expectedCallCount);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_OPERATION);

    // Integer client formats reject floating-point types.
    MG_Impl::GLImpl::ReadPixels(0, 0, 4, 4, GL_GREEN_INTEGER, GL_FLOAT, pixelStorage);
    EXPECT_EQ(g_readPixelsCallCount, expectedCallCount);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_OPERATION);
}

TEST_F(FramebufferTest, NamedRenderbufferStorageAndFramebufferAttachDoNotChangeBindings) {
    GLuint framebuffer = 0;
    GLuint renderbuffer = 0;
    MG_Impl::GLImpl::CreateFramebuffers(1, &framebuffer);
    MG_Impl::GLImpl::CreateRenderbuffers(1, &renderbuffer);

    const auto originalDraw =
        MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject();
    const auto originalRead =
        MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read).GetBoundObject();
    const auto originalRenderbuffer =
        MG_State::pGLContext->GetRenderbufferBindingSlot(RenderbufferTarget::Renderbuffer).GetBoundObject();

    MG_Impl::GLImpl::NamedRenderbufferStorage(renderbuffer, GL_RGBA8, 64, 32);

    GLint width = 0;
    GLint height = 0;
    GLint format = 0;
    MG_Impl::GLImpl::GetNamedRenderbufferParameteriv(renderbuffer, GL_RENDERBUFFER_WIDTH, &width);
    MG_Impl::GLImpl::GetNamedRenderbufferParameteriv(renderbuffer, GL_RENDERBUFFER_HEIGHT, &height);
    MG_Impl::GLImpl::GetNamedRenderbufferParameteriv(renderbuffer, GL_RENDERBUFFER_INTERNAL_FORMAT, &format);

    EXPECT_EQ(width, 64);
    EXPECT_EQ(height, 32);
    EXPECT_EQ(format, GL_RGBA8);

    MG_Impl::GLImpl::NamedFramebufferRenderbuffer(framebuffer, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, renderbuffer);

    const auto framebufferObject = MG_State::pGLContext->GetFramebufferObject(framebuffer);
    const auto& attachment = framebufferObject->GetAttachment(FramebufferAttachmentType::Color0);
    EXPECT_TRUE(attachment.IsRenderbuffer());
    EXPECT_EQ(attachment.GetRenderbuffer()->GetExternalIndex(), renderbuffer);
    EXPECT_EQ(MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject(), originalDraw);
    EXPECT_EQ(MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read).GetBoundObject(), originalRead);
    EXPECT_EQ(MG_State::pGLContext->GetRenderbufferBindingSlot(RenderbufferTarget::Renderbuffer).GetBoundObject(),
              originalRenderbuffer);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(FramebufferTest, NamedFramebufferDrawBuffersDoNotModifyDefaultFramebuffer) {
    GLuint framebuffer = 0;
    MG_Impl::GLImpl::CreateFramebuffers(1, &framebuffer);

    const auto defaultDraw =
        MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject();
    const auto defaultRead =
        MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read).GetBoundObject();
    const auto defaultDrawBuffer = defaultDraw->GetDrawBuffers()[0];
    const auto defaultReadBuffer = defaultRead->GetReadBuffer();

    GLenum bufs[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    MG_Impl::GLImpl::NamedFramebufferDrawBuffers(framebuffer, 2, bufs);
    MG_Impl::GLImpl::NamedFramebufferReadBuffer(framebuffer, GL_COLOR_ATTACHMENT1);

    const auto framebufferObject = MG_State::pGLContext->GetFramebufferObject(framebuffer);
    EXPECT_EQ(framebufferObject->GetDrawBuffers()[0], FramebufferAttachmentType::Color0);
    EXPECT_EQ(framebufferObject->GetDrawBuffers()[1], FramebufferAttachmentType::Color1);
    EXPECT_EQ(framebufferObject->GetReadBuffer(), FramebufferAttachmentType::Color1);
    EXPECT_EQ(defaultDraw->GetDrawBuffers()[0], defaultDrawBuffer);
    EXPECT_EQ(defaultRead->GetReadBuffer(), defaultReadBuffer);
    EXPECT_EQ(MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject(), defaultDraw);
    EXPECT_EQ(MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read).GetBoundObject(), defaultRead);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(FramebufferTest, DefaultFramebufferReadBufferAcceptsGLBackAlias) {
    const auto defaultRead =
        MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read).GetBoundObject();

    MG_Impl::GLImpl::ReadBuffer(GL_BACK);

    EXPECT_EQ(defaultRead->GetReadBuffer(), FramebufferAttachmentType::BackLeft);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(FramebufferTest, DefaultFramebufferDrawBufferAcceptsGLFrontAlias) {
    const auto defaultDraw =
        MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject();

    MG_Impl::GLImpl::DrawBuffer(GL_FRONT);

    EXPECT_EQ(defaultDraw->GetDrawBuffers()[0], FramebufferAttachmentType::FrontLeft);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(FramebufferTest, DefaultFramebufferProvidesTextureAttachmentsForFrontAndBackAliases) {
    const auto defaultFramebuffer =
        MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject();
    ASSERT_NE(defaultFramebuffer, nullptr);

    const auto& frontLeft = defaultFramebuffer->GetAttachment(FramebufferAttachmentType::FrontLeft);
    const auto& frontRight = defaultFramebuffer->GetAttachment(FramebufferAttachmentType::FrontRight);
    const auto& backLeft = defaultFramebuffer->GetAttachment(FramebufferAttachmentType::BackLeft);
    const auto& backRight = defaultFramebuffer->GetAttachment(FramebufferAttachmentType::BackRight);

    EXPECT_TRUE(frontLeft.IsTexture());
    EXPECT_TRUE(frontRight.IsTexture());
    EXPECT_TRUE(backLeft.IsTexture());
    EXPECT_TRUE(backRight.IsTexture());
    EXPECT_TRUE(frontLeft.IsComplete());
    EXPECT_TRUE(frontRight.IsComplete());
    EXPECT_TRUE(backLeft.IsComplete());
    EXPECT_TRUE(backRight.IsComplete());
}

TEST_F(FramebufferTest, ClearNamedFramebufferfvUsesNamedObjectWithoutChangingBindings) {
    GLuint framebuffer = 0;
    MG_Impl::GLImpl::CreateFramebuffers(1, &framebuffer);

    const auto defaultDraw =
        MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject();
    const auto defaultRead =
        MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read).GetBoundObject();

    const GLfloat depth[] = {0.25f};
    MG_Backend::gBackendFunctionsTable.GL.ClearNamedFramebufferfv = RecordClearNamedFramebufferfv;
    MG_Impl::GLImpl::ClearNamedFramebufferfv(framebuffer, GL_DEPTH, 0, depth);

    const auto framebufferObject = MG_State::pGLContext->GetFramebufferObject(framebuffer);
    EXPECT_EQ(g_clearNamedFramebufferfvCallCount, 1);
    EXPECT_EQ(g_lastClearFramebuffer, framebufferObject);
    EXPECT_EQ(g_lastClearBuffer, GL_DEPTH);
    EXPECT_EQ(g_lastClearDrawbuffer, 0);
    EXPECT_EQ(g_lastClearDepth, 0.25f);
    EXPECT_EQ(MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject(), defaultDraw);
    EXPECT_EQ(MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read).GetBoundObject(), defaultRead);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(FramebufferTest, ClearNamedFramebufferfiAllowsDefaultFramebufferZero) {
    GLuint framebuffer = 0;
    MG_Impl::GLImpl::CreateFramebuffers(1, &framebuffer);

    const auto defaultDraw =
        MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject();
    const auto defaultRead =
        MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read).GetBoundObject();

    MG_Backend::gBackendFunctionsTable.GL.ClearNamedFramebufferfi = RecordClearNamedFramebufferfi;
    MG_Impl::GLImpl::ClearNamedFramebufferfi(0, GL_DEPTH_STENCIL, 0, 0.5f, 7);

    EXPECT_EQ(g_clearNamedFramebufferfiCallCount, 1);
    EXPECT_EQ(g_lastClearFramebuffer, defaultDraw);
    EXPECT_EQ(g_lastClearBuffer, GL_DEPTH_STENCIL);
    EXPECT_EQ(g_lastClearDrawbuffer, 0);
    EXPECT_EQ(g_lastClearDepth, 0.5f);
    EXPECT_EQ(g_lastClearStencil, 7);
    EXPECT_EQ(MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject(), defaultDraw);
    EXPECT_EQ(MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read).GetBoundObject(), defaultRead);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(FramebufferTest, GetNamedFramebufferAttachmentParameterivReadsTargetObjectDirectly) {
    GLuint framebuffer = 0;
    MG_Impl::GLImpl::CreateFramebuffers(1, &framebuffer);

    Vector<Uint> textureNames;
    MG_State::pGLContext->GenTextureNames(1, textureNames);
    MG_State::pGLContext->CreateTextureObject(textureNames[0], TextureTarget::Texture2D);
    MG_Impl::GLImpl::NamedFramebufferTexture(framebuffer, GL_COLOR_ATTACHMENT0, textureNames[0], 2);

    GLint objectType = 0;
    GLint objectName = 0;
    GLint textureLevel = 0;
    MG_Impl::GLImpl::GetNamedFramebufferAttachmentParameteriv(
        framebuffer, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &objectType);
    MG_Impl::GLImpl::GetNamedFramebufferAttachmentParameteriv(
        framebuffer, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &objectName);
    MG_Impl::GLImpl::GetNamedFramebufferAttachmentParameteriv(
        framebuffer, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL, &textureLevel);

    EXPECT_EQ(objectType, GL_TEXTURE);
    EXPECT_EQ(objectName, static_cast<GLint>(textureNames[0]));
    EXPECT_EQ(textureLevel, 2);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(FramebufferTest, BlitNamedFramebufferAllowsDefaultFramebufferZero) {
    GLuint framebuffer = 0;
    MG_Impl::GLImpl::CreateFramebuffers(1, &framebuffer);

    const auto defaultDraw =
        MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject();
    const auto defaultRead =
        MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read).GetBoundObject();

    MG_Backend::gBackendFunctionsTable.GL.BlitNamedFramebuffer = RecordBlitNamedFramebuffer;
    MG_Impl::GLImpl::BlitNamedFramebuffer(framebuffer, 0, 0, 0, 16, 16, 0, 0, 16, 16, GL_COLOR_BUFFER_BIT,
                                          GL_NEAREST);

    const auto framebufferObject = MG_State::pGLContext->GetFramebufferObject(framebuffer);
    EXPECT_EQ(g_blitNamedFramebufferCallCount, 1);
    EXPECT_EQ(g_lastBlitReadFramebuffer, framebufferObject);
    EXPECT_EQ(g_lastBlitDrawFramebuffer, defaultDraw);
    EXPECT_EQ(MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject(), defaultDraw);
    EXPECT_EQ(MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read).GetBoundObject(), defaultRead);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}
