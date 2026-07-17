// MobileGL - MobileGL/MG_Test/Framebuffer/FramebufferTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <gtest/gtest.h>

#include <limits>

#include "Includes.h"
#include "Init.h"
#include <MG_Backend/BackendObjects.h>
#include <MG_Backend/DirectGLES/Utils.h>
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
    // GL error flags are sticky per error code and the context outlives an individual test in this
    // binary, so drain whatever an earlier test left pending - otherwise an error-code assertion
    // here reads someone else's error. Bounded: one flag per code, so this cannot hang the suite.
    static void DrainPendingGlErrors() {
        for (Int drained = 0; drained < 16 && MG_Impl::GLImpl::GetError() != GL_NO_ERROR; ++drained) {
        }
    }

    // The call under test must raise exactly the expected error and nothing more: a second pending
    // error means one entry point queued several, which GetError() would hand out at an unrelated
    // call site later on.
    static void ExpectSingleGlError(GLenum expected) {
        EXPECT_EQ(MG_Impl::GLImpl::GetError(), expected);
        EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR) << "the call recorded more than one error";
    }

    void TearDown() override {
        // Attribute a leaked error to the test that caused it instead of to whoever runs next.
        EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR) << "test left an unconsumed GL error behind";
    }

    void SetUp() override {
        MobileGL::Initialize();
        DrainPendingGlErrors();
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

// GL 3.3 core 4.4.1/4.4.2 name lifecycle - mirrors the rules asserted for the other object
// families: deleting an unknown name is silent, a released reservation is recycled, and binding
// a dead name is INVALID_OPERATION.
TEST_F(FramebufferTest, DeleteOfUnknownOrAlreadyDeletedFramebufferNameIsSilent) {
    GLuint framebuffer = 0;
    MG_Impl::GLImpl::GenFramebuffers(1, &framebuffer);
    ASSERT_NE(framebuffer, 0u);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::DeleteFramebuffers(1, &framebuffer);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::DeleteFramebuffers(1, &framebuffer);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // Not a small literal: other tests in this binary share the context and generate names in
    // bulk, so a low number may well be a legitimately reserved name here.
    const GLuint unknownNames[] = {0u, std::numeric_limits<GLuint>::max()};
    MG_Impl::GLImpl::DeleteFramebuffers(2, unknownNames);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(FramebufferTest, DeleteGeneratedButUnboundFramebufferNameReleasesReservationAndBindFails) {
    GLuint framebuffer = 0;
    MG_Impl::GLImpl::GenFramebuffers(1, &framebuffer);
    ASSERT_NE(framebuffer, 0u);
    ASSERT_TRUE(MG_State::pGLContext->ValidateFramebufferName(framebuffer));

    MG_Impl::GLImpl::DeleteFramebuffers(1, &framebuffer);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_FALSE(MG_State::pGLContext->ValidateFramebufferName(framebuffer));

    MG_Impl::GLImpl::BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    ExpectSingleGlError(GL_INVALID_OPERATION);

    GLuint recycled = 0;
    MG_Impl::GLImpl::GenFramebuffers(1, &recycled);
    EXPECT_EQ(recycled, framebuffer);
}

TEST_F(FramebufferTest, DeleteOfUnknownOrAlreadyDeletedRenderbufferNameIsSilent) {
    GLuint renderbuffer = 0;
    MG_Impl::GLImpl::GenRenderbuffers(1, &renderbuffer);
    ASSERT_NE(renderbuffer, 0u);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::DeleteRenderbuffers(1, &renderbuffer);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::DeleteRenderbuffers(1, &renderbuffer);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // Not a small literal: other tests in this binary share the context and generate names in
    // bulk, so a low number may well be a legitimately reserved name here.
    const GLuint unknownNames[] = {0u, std::numeric_limits<GLuint>::max()};
    MG_Impl::GLImpl::DeleteRenderbuffers(2, unknownNames);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(FramebufferTest, DeleteGeneratedButUnboundRenderbufferNameReleasesReservationAndBindFails) {
    GLuint renderbuffer = 0;
    MG_Impl::GLImpl::GenRenderbuffers(1, &renderbuffer);
    ASSERT_NE(renderbuffer, 0u);
    ASSERT_TRUE(MG_State::pGLContext->ValidateRenderbufferName(renderbuffer));

    MG_Impl::GLImpl::DeleteRenderbuffers(1, &renderbuffer);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_FALSE(MG_State::pGLContext->ValidateRenderbufferName(renderbuffer));

    MG_Impl::GLImpl::BindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
    ExpectSingleGlError(GL_INVALID_OPERATION);

    GLuint recycled = 0;
    MG_Impl::GLImpl::GenRenderbuffers(1, &recycled);
    EXPECT_EQ(recycled, renderbuffer);
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

// ---- Packed-type readback encoding ------------------------------------------------------------------
// Oracle-independent guard for the DirectGLES client-format readback conversion: feeds known wide RGBA
// rows through ReadbackImpl::ConvertWideReadbackRow and asserts the exact packed words. Field positions
// were hand-computed from GL 3.3 table 3.6 and match the GL CTS packed_pixels comparison functions
// (glcPackedPixelsTests.cpp pack_UNSIGNED_*): non-REV types pack the first format component from the
// most significant bit, *_REV types from the least significant bit.

namespace {
    namespace ReadbackImpl = MG_Backend::DirectGLES::ReadbackImpl;

    // Converts a row of wide pixels (4 components of wideType each) into `format`/`type` words.
    template <typename WordT, typename SrcT>
    Vector<WordT> ConvertWideRowToPackedWords(const Vector<SrcT>& wide, GLenum wideType, GLenum format,
                                              GLenum type) {
        ReadbackImpl::ReadbackChannelMapping mapping{};
        EXPECT_TRUE(ReadbackImpl::GetReadbackChannelMapping(format, mapping));
        EXPECT_EQ(ReadbackImpl::GetReadbackDstPixelSize(mapping, type), sizeof(WordT));
        const SizeT width = wide.size() / 4;
        Vector<WordT> out(width, static_cast<WordT>(0));
        ReadbackImpl::ConvertWideReadbackRow(reinterpret_cast<const Uint8*>(wide.data()),
                                             reinterpret_cast<Uint8*>(out.data()), width, wideType, mapping,
                                             type);
        return out;
    }

    // Normalized encodes read the wide row as RGBA8 (values are v / 255).
    template <typename WordT>
    Vector<WordT> ConvertRGBA8Row(const Vector<Uint8>& rgba, GLenum format, GLenum type) {
        return ConvertWideRowToPackedWords<WordT>(rgba, GL_UNSIGNED_BYTE, format, type);
    }

    // Wide RGBA8 pattern shared by the normalized-encode tests. Expected fields below are
    // round(v / 255 * (2^bits - 1)), computed by hand per pixel.
    //                            R     G     B     A
    const Vector<Uint8> kRGBA8Row{255,  0,    128,  64,     // P0
                                  10,   250,  33,   200,    // P1
                                  85,   170,  255,  0};     // P2
} // namespace

TEST(PackedReadbackEncodeTest, EncodesUnsignedShort565) {
    // P0: R=31 G=0 B=round(128*31/255)=16      -> 31<<11 | 0<<5 | 16      = 0xF810
    // P1: R=round(10*31/255)=1 G=round(250*63/255)=62 B=round(33*31/255)=4 -> 1<<11|62<<5|4 = 0x0FC4
    // P2: R=round(85*31/255)=10 G=round(170*63/255)=42 B=31               -> 10<<11|42<<5|31 = 0x555F
    const auto words = ConvertRGBA8Row<Uint16>(kRGBA8Row, GL_RGB, GL_UNSIGNED_SHORT_5_6_5);
    EXPECT_EQ(words[0], 0xF810u);
    EXPECT_EQ(words[1], 0x0FC4u);
    EXPECT_EQ(words[2], 0x555Fu);
}

TEST(PackedReadbackEncodeTest, EncodesUnsignedShort565Rev) {
    // REV packs R from the LSB: P2 -> 10 | 42<<5 | 31<<11 = 0xFD4A
    const auto words = ConvertRGBA8Row<Uint16>(kRGBA8Row, GL_RGB, GL_UNSIGNED_SHORT_5_6_5_REV);
    EXPECT_EQ(words[2], 0xFD4Au);
}

TEST(PackedReadbackEncodeTest, EncodesUnsignedShort4444) {
    // P0: R=15 G=0 B=round(128*15/255)=8 A=round(64*15/255)=4 -> 0xF084
    // P2: R=round(85*15/255)=5 G=round(170*15/255)=10 B=15 A=0 -> 0x5AF0
    const auto words = ConvertRGBA8Row<Uint16>(kRGBA8Row, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4);
    EXPECT_EQ(words[0], 0xF084u);
    EXPECT_EQ(words[2], 0x5AF0u);
}

TEST(PackedReadbackEncodeTest, EncodesUnsignedShort4444Rev) {
    // P0 fields R=15 G=0 B=8 A=4 packed from the LSB -> 15 | 0<<4 | 8<<8 | 4<<12 = 0x480F
    const auto words = ConvertRGBA8Row<Uint16>(kRGBA8Row, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4_REV);
    EXPECT_EQ(words[0], 0x480Fu);
}

TEST(PackedReadbackEncodeTest, EncodesUnsignedShort5551) {
    // P0: R=31 G=0 B=16 A=round(64/255)=0  -> 31<<11 | 16<<1        = 0xF820
    // P1: R=1 G=round(250*31/255)=30 B=4 A=round(200/255)=1 -> 1<<11|30<<6|4<<1|1 = 0x0F89
    const auto words = ConvertRGBA8Row<Uint16>(kRGBA8Row, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1);
    EXPECT_EQ(words[0], 0xF820u);
    EXPECT_EQ(words[1], 0x0F89u);
}

TEST(PackedReadbackEncodeTest, EncodesUnsignedShort1555Rev) {
    // P1 fields R=1 G=30 B=4 A=1 packed from the LSB -> 1 | 30<<5 | 4<<10 | 1<<15 = 0x93C1
    const auto words = ConvertRGBA8Row<Uint16>(kRGBA8Row, GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV);
    EXPECT_EQ(words[1], 0x93C1u);
}

TEST(PackedReadbackEncodeTest, EncodesUnsignedInt2101010Rev) {
    // P0: R=1023 G=0 B=round(128*1023/255)=514 A=round(64*3/255)=1 -> 1023|514<<20|1<<30 = 0x602003FF
    // P2: R=round(85*1023/255)=341 G=round(170*1023/255)=682 B=1023 A=0 -> 0x3FFAA955
    const auto words = ConvertRGBA8Row<Uint32>(kRGBA8Row, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV);
    EXPECT_EQ(words[0], 0x602003FFu);
    EXPECT_EQ(words[2], 0x3FFAA955u);
}

TEST(PackedReadbackEncodeTest, EncodesUnsignedInt1010102) {
    // P2 fields R=341 G=682 B=1023 A=0 packed from the MSB -> 341<<22 | 682<<12 | 1023<<2 = 0x556AAFFC
    const auto words = ConvertRGBA8Row<Uint32>(kRGBA8Row, GL_RGBA, GL_UNSIGNED_INT_10_10_10_2);
    EXPECT_EQ(words[2], 0x556AAFFCu);
}

TEST(PackedReadbackEncodeTest, EncodesUnsignedByte332) {
    // P0: R=7 G=0 B=round(128*3/255)=2                        -> 7<<5 | 2   = 0xE2
    // P1: R=round(10*7/255)=0 G=round(250*7/255)=7 B=round(33*3/255)=0 -> 7<<2 = 0x1C
    const auto words = ConvertRGBA8Row<Uint8>(kRGBA8Row, GL_RGB, GL_UNSIGNED_BYTE_3_3_2);
    EXPECT_EQ(words[0], 0xE2u);
    EXPECT_EQ(words[1], 0x1Cu);
}

TEST(PackedReadbackEncodeTest, EncodesUnsignedByte233Rev) {
    // P0 fields R=7 G=0 B=2 packed from the LSB -> 7 | 0<<3 | 2<<6 = 0x87
    const auto words = ConvertRGBA8Row<Uint8>(kRGBA8Row, GL_RGB, GL_UNSIGNED_BYTE_2_3_3_REV);
    EXPECT_EQ(words[0], 0x87u);
}

TEST(PackedReadbackEncodeTest, Encodes8888KeepsLegacyByteOrder) {
    // Regression for the previously supported types: P0 = (255, 0, 128, 64).
    const auto msbFirst = ConvertRGBA8Row<Uint32>(kRGBA8Row, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8);
    EXPECT_EQ(msbFirst[0], 0xFF008040u);
    const auto lsbFirst = ConvertRGBA8Row<Uint32>(kRGBA8Row, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8_REV);
    EXPECT_EQ(lsbFirst[0], 0x408000FFu);
}

TEST(PackedReadbackEncodeTest, EncodesBGRAWithChannelMapping) {
    // BGRA's first format component is Blue: P0 fields B=8 G=0 R=15 A=4 -> 8<<12 | 15<<4 | 4 = 0x80F4
    const auto words = ConvertRGBA8Row<Uint16>(kRGBA8Row, GL_BGRA, GL_UNSIGNED_SHORT_4_4_4_4);
    EXPECT_EQ(words[0], 0x80F4u);
}

TEST(PackedReadbackEncodeTest, EncodesIntegerRGBA2101010RevWithFieldClamp) {
    // Integer sources clamp to each field's unsigned range (10/10/10/2 bits).
    const Vector<Uint32> wide{1023u, 1024u, 5u, 4u};
    const auto words =
        ConvertWideRowToPackedWords<Uint32>(wide, GL_UNSIGNED_INT, GL_RGBA_INTEGER, GL_UNSIGNED_INT_2_10_10_10_REV);
    EXPECT_EQ(words[0], 0xC05FFFFFu); // 1023 | 1023<<10 | 5<<20 | 3<<30
}

TEST(PackedReadbackEncodeTest, EncodesIntegerNegativeValuesClampToZero) {
    const Vector<Int32> wide{-5, 2, 100000, 1};
    const auto words =
        ConvertWideRowToPackedWords<Uint32>(wide, GL_INT, GL_RGBA_INTEGER, GL_UNSIGNED_INT_2_10_10_10_REV);
    EXPECT_EQ(words[0], 0x7FF00800u); // 0 | 2<<10 | 1023<<20 | 1<<30
}

TEST(PackedReadbackEncodeTest, EncodesIntegerRGB565) {
    const Vector<Uint32> wide{31u, 64u, 2u, 0u};
    const auto words =
        ConvertWideRowToPackedWords<Uint16>(wide, GL_UNSIGNED_INT, GL_RGB_INTEGER, GL_UNSIGNED_SHORT_5_6_5);
    EXPECT_EQ(words[0], 0xFFE2u); // 31<<11 | 63<<5 | 2 (G clamps 64 -> 63)
}

TEST(PackedReadbackEncodeTest, EncodesPackedFloat10F11F11FRev) {
    // F11(1.0)=0x3C0 F11(0.5)=0x380 F10(0.25)=0x1A0 -> 0x3C0 | 0x380<<11 | 0x1A0<<22 = 0x681C03C0.
    // Second pixel: values above 65024 clamp to the max finite F11 (0x7BF), negatives go to zero.
    const Vector<Float> wide{1.0f, 0.5f, 0.25f, 1.0f, 100000.0f, -1.0f, 0.25f, 1.0f};
    const auto words = ConvertWideRowToPackedWords<Uint32>(wide, GL_FLOAT, GL_RGB, GL_UNSIGNED_INT_10F_11F_11F_REV);
    EXPECT_EQ(words[0], 0x681C03C0u);
    EXPECT_EQ(words[1], 0x680007BFu);
}

TEST(PackedReadbackEncodeTest, EncodesSharedExponent5999Rev) {
    // (1.0, 0.5, 0.25): shared exponent 16, fields 256/128/64 -> 256 | 128<<9 | 64<<18 | 16<<27
    const Vector<Float> wide{1.0f, 0.5f, 0.25f, 1.0f};
    const auto words = ConvertWideRowToPackedWords<Uint32>(wide, GL_FLOAT, GL_RGB, GL_UNSIGNED_INT_5_9_9_9_REV);
    EXPECT_EQ(words[0], 0x81010100u);
}

TEST(PackedReadbackEncodeTest, RejectsMismatchedPackedFieldCounts) {
    ReadbackImpl::ReadbackChannelMapping rgba{};
    ASSERT_TRUE(ReadbackImpl::GetReadbackChannelMapping(GL_RGBA, rgba));
    ReadbackImpl::ReadbackChannelMapping rgbInteger{};
    ASSERT_TRUE(ReadbackImpl::GetReadbackChannelMapping(GL_RGB_INTEGER, rgbInteger));

    // 3-field packed types never pair with 4-component formats and vice versa.
    EXPECT_EQ(ReadbackImpl::GetReadbackDstPixelSize(rgba, GL_UNSIGNED_SHORT_5_6_5), 0u);
    EXPECT_EQ(ReadbackImpl::GetReadbackDstPixelSize(rgbInteger, GL_UNSIGNED_SHORT_4_4_4_4), 0u);
    // Packed-float RGB types never pair with integer formats.
    EXPECT_EQ(ReadbackImpl::GetReadbackDstPixelSize(rgbInteger, GL_UNSIGNED_INT_5_9_9_9_REV), 0u);
    EXPECT_EQ(ReadbackImpl::GetReadbackDstPixelSize(rgbInteger, GL_UNSIGNED_INT_10F_11F_11F_REV), 0u);
}

// ---- GL CTS packed_pixels readback root-cause regressions --------------------------------------

TEST_F(FramebufferTest, ReadPixelsRejectsIntegerFormatMismatchWithReadBuffer) {
    GLuint framebuffer = 0;
    GLuint texture = 0;
    MG_Impl::GLImpl::CreateFramebuffers(1, &framebuffer);
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &texture);
    MG_Impl::GLImpl::TextureStorage2D(texture, 1, GL_RGBA8UI, 4, 4);
    MG_Impl::GLImpl::NamedFramebufferTexture(framebuffer, GL_COLOR_ATTACHMENT0, texture, 0);
    MG_Impl::GLImpl::BindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);

    MG_Backend::gBackendFunctionsTable.GL.ReadPixels = RecordReadPixels;
    Uint8 pixelStorage[4 * 4 * 4] = {};

    // GL 3.3 section 4.3.1: normalized format on an integer read buffer -> GL_INVALID_OPERATION
    // (GL CTS packed_pixels expects the error for every mismatched combination).
    MG_Impl::GLImpl::ReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, pixelStorage);
    EXPECT_EQ(g_readPixelsCallCount, 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_OPERATION);

    // The matching integer readback stays valid.
    MG_Impl::GLImpl::ReadPixels(0, 0, 4, 4, GL_RGBA_INTEGER, GL_UNSIGNED_INT, pixelStorage);
    EXPECT_EQ(g_readPixelsCallCount, 1);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // And the inverse mismatch: integer format on a normalized attachment.
    GLuint normalizedTexture = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &normalizedTexture);
    MG_Impl::GLImpl::TextureStorage2D(normalizedTexture, 1, GL_RGBA8, 4, 4);
    MG_Impl::GLImpl::NamedFramebufferTexture(framebuffer, GL_COLOR_ATTACHMENT0, normalizedTexture, 0);
    MG_Impl::GLImpl::ReadPixels(0, 0, 4, 4, GL_RGBA_INTEGER, GL_UNSIGNED_INT, pixelStorage);
    EXPECT_EQ(g_readPixelsCallCount, 1);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_OPERATION);
}

TEST_F(FramebufferTest, BindRenderbufferZeroUnbindsWithoutError) {
    // The GL CTS state reset calls glBindRenderbuffer(GL_RENDERBUFFER, 0) and expects no error;
    // name 0 used to be reported as an invalid renderbuffer name.
    MG_Impl::GLImpl::BindRenderbuffer(GL_RENDERBUFFER, 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(FramebufferTest, FramebufferTexture3DAttachesSliceWithLayerTracking) {
    // glFramebufferTexture3D with zoffset used to be rejected outright, leaving a sticky
    // GL_INVALID_OPERATION behind (GL CTS packed_pixels varied_rectangle runs on GL_TEXTURE_3D).
    GLuint framebuffer = 0;
    GLuint texture = 0;
    MG_Impl::GLImpl::CreateFramebuffers(1, &framebuffer);
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_3D, 1, &texture);
    MG_Impl::GLImpl::TextureStorage3D(texture, 1, GL_RGBA8, 4, 4, 2);

    MG_Impl::GLImpl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
    MG_Impl::GLImpl::FramebufferTexture3D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_3D, texture, 0, 1);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    const auto framebufferObject = MG_State::pGLContext->GetFramebufferObject(framebuffer);
    ASSERT_NE(framebufferObject, nullptr);
    const auto& attachment = framebufferObject->GetAttachment(FramebufferAttachmentType::Color0);
    ASSERT_TRUE(attachment.IsTexture());
    EXPECT_EQ(attachment.GetTextureLayer(), 1);
    EXPECT_FALSE(attachment.IsLayered());
}

TEST_F(FramebufferTest, NonRenderableColorFormatsReportUnsupportedFramebuffer) {
    // Without a probing backend the conservative list applies: RGB9_E5 is texture-only, so
    // attaching it must not report GL_FRAMEBUFFER_COMPLETE (GL CTS packed_pixels rgb9_e5 expects
    // read errors instead of silent unwritten readbacks).
    GLuint framebuffer = 0;
    GLuint texture = 0;
    MG_Impl::GLImpl::CreateFramebuffers(1, &framebuffer);
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &texture);
    MG_Impl::GLImpl::TextureStorage2D(texture, 1, GL_RGB9_E5, 4, 4);
    MG_Impl::GLImpl::NamedFramebufferTexture(framebuffer, GL_COLOR_ATTACHMENT0, texture, 0);
    MG_Impl::GLImpl::BindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);

    EXPECT_EQ(MG_Impl::GLImpl::CheckFramebufferStatus(GL_READ_FRAMEBUFFER),
              static_cast<GLenum>(GL_FRAMEBUFFER_UNSUPPORTED));

    Uint8 pixelStorage[4 * 4 * 4] = {};
    MG_Impl::GLImpl::ReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, pixelStorage);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_FRAMEBUFFER_OPERATION);
}
