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
#include <MG_Backend/DirectGLES/DirectGLES.h>
#include <MG_Backend/DirectGLES/Managers.h>
#include <MG_Backend/DirectGLES/Utils.h>
#include <MG_Impl/GLImpl/Buffer/GL_Buffer.h>
#include <MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.h>
#include <MG_Impl/GLImpl/Getter/GL_Getter.h>
#include <MG_Impl/GLImpl/RenderState/GL_RenderState.h>
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

// ---- Three-channel colour attachments: the Complementary Reimagined / Iris load failure --------
//
// Complementary declares colortex1 = RGB8_SNORM and colortex2 = RGB16F. No real OpenGL ES driver
// renders to a three-channel image (EXT_render_snorm covers R/RG/RGBA only; EXT_color_buffer_float
// excludes RGB16F), so the DirectGLES probe records those formats as creatable-but-not-renderable
// and the frontend answered every framebuffer built from them GL_FRAMEBUFFER_UNSUPPORTED - which
// Iris turns into a hard "Draw buffers [0, 1] Status: 36061" load failure. The backend now records
// the four-channel substitution it will actually allocate as a caveat capability, and the frontend
// has to accept that as renderable.
namespace {
    class ThreeChannelAttachmentBackend final : public MG_Backend::BackendObject {
    public:
        // `substituted` stands in for a driver where the four-channel widening probe succeeded, i.e.
        // for what PopulateFormatCapabilitiesImpl records on Mali. false is the pre-fix state: the
        // native form is creatable, nothing is renderable, and no fallback was ever built.
        explicit ThreeChannelAttachmentBackend(Bool substituted) {
            auto& cache = MutableFormatCapabilities();
            const auto texture2DIndex = MG_Backend::GetFormatCapabilityTargetIndex(TextureTarget::Texture2D);

            // IsColorInternalFormatRenderable only trusts the cache once it looks populated, which
            // it decides from RGBA8 being creatable somewhere. Without this the static deny-list
            // answers instead and the caveat below would never be consulted.
            const auto rgba8Index = static_cast<SizeT>(TextureInternalFormat::RGBA8);
            cache.FullCaps[texture2DIndex][rgba8Index] |= MG_Backend::FormatCapability::Creatable;
            cache.FullCaps[texture2DIndex][rgba8Index] |= MG_Backend::FormatCapability::FramebufferRenderable;
            cache.FullCaps[texture2DIndex][rgba8Index] |= MG_Backend::FormatCapability::ColorAttachment;

            for (const TextureInternalFormat format :
                 {TextureInternalFormat::RGB8Snorm, TextureInternalFormat::RGB16F}) {
                const auto formatIndex = static_cast<SizeT>(format);
                // Creatable and samplable as an ordinary texture, but the driver's
                // glCheckFramebufferStatus said no - exactly Mali r32p1's answer.
                cache.FullCaps[texture2DIndex][formatIndex] |= MG_Backend::FormatCapability::Creatable;
                cache.FullCaps[texture2DIndex][formatIndex] |= MG_Backend::FormatCapability::Sampled;
                if (substituted) {
                    cache.CaveatCaps[texture2DIndex][formatIndex] |=
                        MG_Backend::FormatCapability::FramebufferRenderable;
                    cache.CaveatCaps[texture2DIndex][formatIndex] |= MG_Backend::FormatCapability::ColorAttachment;
                }
            }
        }

        void Initialize() override {}
        Bool InitCapabilities() override { return true; }
        Bool InitWindowSurface() override { return true; }
        const RendererInfo& GetRendererInfo() const override {
            static RendererInfo info = {};
            return info;
        }
        String GetBackendAPIVersionString() const override { return {}; }
        const MG_Backend::GlobalBackendFunctionsTable& GetBackendFunctions() const override {
            static MG_Backend::GlobalBackendFunctionsTable table = {};
            return table;
        }
        const MG_Backend::DynamicBackendParameters& GetDynamicParameters() const override {
            static MG_Backend::DynamicBackendParameters params = {};
            return params;
        }
        BackendType GetBackendType() const override { return BackendType::Unknown; }
    };

    class ScopedBackendOverride {
    public:
        explicit ScopedBackendOverride(UniquePtr<MG_Backend::BackendObject> backend):
            m_previous(Move(MG_Backend::pActiveBackendObject)) {
            MG_Backend::pActiveBackendObject = Move(backend);
        }

        ~ScopedBackendOverride() { MG_Backend::pActiveBackendObject = Move(m_previous); }

    private:
        UniquePtr<MG_Backend::BackendObject> m_previous;
    };

    GLenum CheckSingleColorAttachmentStatus(GLenum internalFormat) {
        GLuint framebuffer = 0;
        GLuint texture = 0;
        MG_Impl::GLImpl::CreateFramebuffers(1, &framebuffer);
        MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &texture);
        MG_Impl::GLImpl::TextureStorage2D(texture, 1, internalFormat, 4, 4);
        MG_Impl::GLImpl::NamedFramebufferTexture(framebuffer, GL_COLOR_ATTACHMENT0, texture, 0);
        return MG_Impl::GLImpl::CheckNamedFramebufferStatus(framebuffer, GL_DRAW_FRAMEBUFFER);
    }
} // namespace

TEST_F(FramebufferTest, ThreeChannelColorAttachmentsAreUnsupportedWithoutTheWidenedSubstitution) {
    // The pre-fix behaviour, pinned so a regression is a red test rather than a shaderpack that
    // silently stops loading: no caveat capability, so nothing makes these renderable.
    ScopedBackendOverride backend(MakeUnique<ThreeChannelAttachmentBackend>(/*substituted=*/false));

    EXPECT_EQ(CheckSingleColorAttachmentStatus(GL_RGB8_SNORM), static_cast<GLenum>(GL_FRAMEBUFFER_UNSUPPORTED));
    EXPECT_EQ(CheckSingleColorAttachmentStatus(GL_RGB16F), static_cast<GLenum>(GL_FRAMEBUFFER_UNSUPPORTED));
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(FramebufferTest, ThreeChannelColorAttachmentsAreCompleteThroughTheWidenedSubstitution) {
    ScopedBackendOverride backend(MakeUnique<ThreeChannelAttachmentBackend>(/*substituted=*/true));

    // Complementary's colortex1 (RGB8_SNORM) and colortex2 (RGB16F): both must come out COMPLETE,
    // because the backend stores them as GL_RGBA16F. Shipping only the first would move the
    // failure one composite pass down instead of fixing it.
    EXPECT_EQ(CheckSingleColorAttachmentStatus(GL_RGB8_SNORM), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
    EXPECT_EQ(CheckSingleColorAttachmentStatus(GL_RGB16F), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(FramebufferTest, TwoAttachmentCompositeFramebufferMatchesIrisComplementaryPass) {
    // The exact framebuffer Iris failed on: Complementary's `composite` pass draws to colortex7
    // (RGBA16F, natively renderable) and colortex1 (RGB8_SNORM, only renderable widened). Iris
    // logs it as "Draw buffers [0, 1]" - a two-attachment FBO, not colortex 0 and 1.
    ScopedBackendOverride backend(MakeUnique<ThreeChannelAttachmentBackend>(/*substituted=*/true));

    GLuint framebuffer = 0;
    GLuint colortex7 = 0;
    GLuint colortex1 = 0;
    MG_Impl::GLImpl::CreateFramebuffers(1, &framebuffer);
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &colortex7);
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &colortex1);
    MG_Impl::GLImpl::TextureStorage2D(colortex7, 1, GL_RGBA8, 4, 4);
    MG_Impl::GLImpl::TextureStorage2D(colortex1, 1, GL_RGB8_SNORM, 4, 4);
    MG_Impl::GLImpl::NamedFramebufferTexture(framebuffer, GL_COLOR_ATTACHMENT0, colortex7, 0);
    MG_Impl::GLImpl::NamedFramebufferTexture(framebuffer, GL_COLOR_ATTACHMENT1, colortex1, 0);

    EXPECT_EQ(MG_Impl::GLImpl::CheckNamedFramebufferStatus(framebuffer, GL_DRAW_FRAMEBUFFER),
              static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));

    // Both entry points answer from the same helpers, and CheckFramebufferStatus is what Iris
    // actually calls; they are near-verbatim duplicates, so assert they agree.
    MG_Impl::GLImpl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
    EXPECT_EQ(MG_Impl::GLImpl::CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER),
              static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// ---- Widened attachments: the stored-alpha discipline -----------------------------------------
//
// A widened attachment has a real alpha channel the application's three-channel format does not,
// and GL says a channel a format lacks reads back as 1.0. glReadPixels and glGetTexImage can be
// made to say that (ForceWideReadAlphaToOne), but GL_DST_ALPHA / GL_ONE_MINUS_DST_ALPHA blending
// and glBlitFramebuffer read the STORED alpha inside the driver, where nothing can intercept it.
// So the stored alpha is held at 1.0 instead: a clear writes 1.0 into it, and every draw has that
// buffer's alpha write mask forced off so nothing can move it again.
//
// These cases pin the two halves of that pairing at the seam where they are visible - what the ES
// driver is actually handed - and pin the invariant that the application's own colour mask is
// never touched.
namespace {
    struct RecordedColorMask {
        Bool seen = false;
        GLboolean r = GL_FALSE, g = GL_FALSE, b = GL_FALSE, a = GL_FALSE;
    };

    constexpr Uint kRecordedDrawBuffers = 8;
    RecordedColorMask g_driverIndexedColorMasks[kRecordedDrawBuffers];
    RecordedColorMask g_driverUniformColorMask;

    void ResetRecordedColorMasks() {
        for (auto& recorded : g_driverIndexedColorMasks) recorded = {};
        g_driverUniformColorMask = {};
    }

    void StubColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
        g_driverUniformColorMask = {true, r, g, b, a};
        // The non-indexed call sets every draw buffer, so record it as such: a later assertion
        // about draw buffer 1 must not read a stale indexed record the uniform push overwrote.
        for (auto& recorded : g_driverIndexedColorMasks) recorded = {true, r, g, b, a};
    }

    void StubColorMaski(GLuint index, GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
        if (index < kRecordedDrawBuffers) g_driverIndexedColorMasks[index] = {true, r, g, b, a};
    }

    // What the blend block of SyncRenderState pushed. Enough to answer the two questions the
    // dual-source cases ask: is blending on for a draw buffer, and which factor enums reached
    // the driver.
    struct RecordedBlend {
        Bool enabled = false;
        Bool enableSeen = false;
        Bool factorsSeen = false;
        GLenum srcRGB = 0, dstRGB = 0, srcAlpha = 0, dstAlpha = 0;
    };
    RecordedBlend g_driverBlend[kRecordedDrawBuffers];

    void ResetRecordedBlend() {
        for (auto& recorded : g_driverBlend) recorded = {};
    }

    void RecordBlendEnable(Bool enabled) {
        for (auto& recorded : g_driverBlend) {
            recorded.enabled = enabled;
            recorded.enableSeen = true;
        }
    }

    void RecordBlendFactors(GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha) {
        for (auto& recorded : g_driverBlend) {
            recorded.factorsSeen = true;
            recorded.srcRGB = srcRGB;
            recorded.dstRGB = dstRGB;
            recorded.srcAlpha = srcAlpha;
            recorded.dstAlpha = dstAlpha;
        }
    }

    void StubViewport(GLint, GLint, GLsizei, GLsizei) {}
    void StubScissor(GLint, GLint, GLsizei, GLsizei) {}
    void StubEnable(GLenum cap) {
        if (cap == GL_BLEND) RecordBlendEnable(true);
    }
    void StubDisable(GLenum cap) {
        if (cap == GL_BLEND) RecordBlendEnable(false);
    }
    void StubEnablei(GLenum cap, GLuint index) {
        if (cap == GL_BLEND && index < kRecordedDrawBuffers) {
            g_driverBlend[index].enabled = true;
            g_driverBlend[index].enableSeen = true;
        }
    }
    void StubDisablei(GLenum cap, GLuint index) {
        if (cap == GL_BLEND && index < kRecordedDrawBuffers) {
            g_driverBlend[index].enabled = false;
            g_driverBlend[index].enableSeen = true;
        }
    }
    void StubBlendFuncSeparate(GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha) {
        RecordBlendFactors(srcRGB, dstRGB, srcAlpha, dstAlpha);
    }
    void StubBlendFuncSeparatei(GLuint index, GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha) {
        if (index >= kRecordedDrawBuffers) return;
        g_driverBlend[index].factorsSeen = true;
        g_driverBlend[index].srcRGB = srcRGB;
        g_driverBlend[index].dstRGB = dstRGB;
        g_driverBlend[index].srcAlpha = srcAlpha;
        g_driverBlend[index].dstAlpha = dstAlpha;
    }
    void StubBlendEquationSeparate(GLenum, GLenum) {}
    void StubBlendEquationSeparatei(GLuint, GLenum, GLenum) {}
    void StubBlendColor(GLfloat, GLfloat, GLfloat, GLfloat) {}
    void StubDepthFunc(GLenum) {}
    void StubDepthMask(GLboolean) {}
    void StubDepthRangef(GLfloat, GLfloat) {}
    void StubStencilFuncSeparate(GLenum, GLenum, GLint, GLuint) {}
    void StubStencilMaskSeparate(GLenum, GLuint) {}
    void StubStencilOpSeparate(GLenum, GLenum, GLenum, GLenum) {}
    void StubClearColor(GLfloat, GLfloat, GLfloat, GLfloat) {}
    void StubClearDepthf(GLfloat) {}
    void StubClearStencil(GLint) {}
    void StubCullFace(GLenum) {}
    void StubFrontFace(GLenum) {}
    void StubPolygonOffset(GLfloat, GLfloat) {}
    void StubLineWidth(GLfloat) {}
    void StubSampleCoverage(GLfloat, GLboolean) {}

    // Replaces the ES function table with no-ops that record only what these cases assert on.
    // The table is ZEROED first on purpose: SyncRenderState is long, and a call it makes that
    // this fixture did not anticipate must crash here rather than silently reach a stale pointer
    // into a driver that this process never made current.
    class ScopedRenderStateDriverStubs {
    public:
        // dualSourceBlendSupported models GL_EXT_blend_func_extended on the ES driver, which is
        // the one capability in here that a real device is commonly WITHOUT.
        explicit ScopedRenderStateDriverStubs(Bool dualSourceBlendSupported = true):
            m_funcs(MG_Backend::DirectGLES::g_GLESFuncs), m_caps(MG_Backend::DirectGLES::g_GLESCapabilities) {
            auto& gl = MG_Backend::DirectGLES::g_GLESFuncs;
            gl = MG_External::GLESFunctionsTable{};
            gl.glViewport = StubViewport;
            gl.glScissor = StubScissor;
            gl.glEnable = StubEnable;
            gl.glDisable = StubDisable;
            gl.glEnablei = StubEnablei;
            gl.glDisablei = StubDisablei;
            gl.glBlendFuncSeparate = StubBlendFuncSeparate;
            gl.glBlendFuncSeparatei = StubBlendFuncSeparatei;
            gl.glBlendEquationSeparate = StubBlendEquationSeparate;
            gl.glBlendEquationSeparatei = StubBlendEquationSeparatei;
            gl.glBlendColor = StubBlendColor;
            gl.glDepthFunc = StubDepthFunc;
            gl.glDepthMask = StubDepthMask;
            gl.glDepthRangef = StubDepthRangef;
            gl.glStencilFuncSeparate = StubStencilFuncSeparate;
            gl.glStencilMaskSeparate = StubStencilMaskSeparate;
            gl.glStencilOpSeparate = StubStencilOpSeparate;
            gl.glClearColor = StubClearColor;
            gl.glClearDepthf = StubClearDepthf;
            gl.glClearStencil = StubClearStencil;
            gl.glCullFace = StubCullFace;
            gl.glFrontFace = StubFrontFace;
            gl.glPolygonOffset = StubPolygonOffset;
            gl.glLineWidth = StubLineWidth;
            gl.glSampleCoverage = StubSampleCoverage;
            gl.glColorMask = StubColorMask;
            gl.glColorMaski = StubColorMaski;

            auto& caps = MG_Backend::DirectGLES::g_GLESCapabilities;
            caps.SupportsIndexedColorMask = true;
            caps.SupportsSrgbWriteControl = false;
            caps.SupportsPolygonMode = false;
            caps.SupportsDualSourceBlend = dualSourceBlendSupported;

            ResetRecordedColorMasks();
            ResetRecordedBlend();
            // The viewport and scissor blocks fall back to querying the surface size when the
            // frontend's rectangle is degenerate, and there is no surface in this process.
            MG_Impl::GLImpl::Viewport(0, 0, 4, 4);
            MG_Impl::GLImpl::Scissor(0, 0, 4, 4);
            MG_Backend::DirectGLES::RenderStateImpl::InvalidateSyncedRenderState();
        }

        ~ScopedRenderStateDriverStubs() {
            MG_Backend::DirectGLES::FramebufferImpl::g_alphaWidenedDrawBufferMask = 0;
            MG_Backend::DirectGLES::g_GLESFuncs = m_funcs;
            MG_Backend::DirectGLES::g_GLESCapabilities = m_caps;
            // The shadow now describes pushes that went to the stubs, not to any driver.
            MG_Backend::DirectGLES::RenderStateImpl::InvalidateSyncedRenderState();
            MG_Impl::GLImpl::ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            // Blend state is per-CONTEXT and the context outlives the fixture, so a case that
            // enabled blending or asked for an exotic factor has to put it back or every later
            // case in this binary inherits it.
            MG_Impl::GLImpl::Disable(GL_BLEND);
            MG_Impl::GLImpl::BlendFunc(GL_ONE, GL_ZERO);
        }

    private:
        MG_External::GLESFunctionsTable m_funcs;
        MG_External::GLESCapabilities m_caps;
    };
} // namespace

TEST_F(FramebufferTest, WidenedDrawBufferIsIdentifiedPerDrawBufferSlotNotPerAttachmentPoint) {
    ScopedBackendOverride backend(MakeUnique<ThreeChannelAttachmentBackend>(/*substituted=*/true));

    // Complementary's `composite` framebuffer again: draw buffer 0 is a natively renderable
    // RGBA8, draw buffer 1 is the widened RGB8_SNORM. Only the second may be doctored.
    GLuint framebuffer = 0;
    GLuint colortex7 = 0;
    GLuint colortex1 = 0;
    MG_Impl::GLImpl::CreateFramebuffers(1, &framebuffer);
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &colortex7);
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &colortex1);
    MG_Impl::GLImpl::TextureStorage2D(colortex7, 1, GL_RGBA8, 4, 4);
    MG_Impl::GLImpl::TextureStorage2D(colortex1, 1, GL_RGB8_SNORM, 4, 4);
    MG_Impl::GLImpl::NamedFramebufferTexture(framebuffer, GL_COLOR_ATTACHMENT0, colortex7, 0);
    MG_Impl::GLImpl::NamedFramebufferTexture(framebuffer, GL_COLOR_ATTACHMENT1, colortex1, 0);

    auto& framebufferObject = MG_State::pGLContext->GetFramebufferObject(framebuffer);
    ASSERT_NE(framebufferObject, nullptr);
    framebufferObject->SetDrawBuffer(0, FramebufferAttachmentType::Color0);
    framebufferObject->SetDrawBuffer(1, FramebufferAttachmentType::Color1);

    EXPECT_EQ(MG_Backend::DirectGLES::FramebufferImpl::ComputeAlphaWidenedDrawBufferMask(*framebufferObject),
              1u << 1);

    // Swapping the draw-buffer array moves the bit with the SLOT, not with the attachment point:
    // glColorMaski and glClearBufferfv both address slots.
    framebufferObject->SetDrawBuffer(0, FramebufferAttachmentType::Color1);
    framebufferObject->SetDrawBuffer(1, FramebufferAttachmentType::Color0);
    EXPECT_EQ(MG_Backend::DirectGLES::FramebufferImpl::ComputeAlphaWidenedDrawBufferMask(*framebufferObject),
              1u << 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(FramebufferTest, DrawIntoAWidenedDrawBufferReachesTheDriverWithAlphaWritesMaskedOff) {
    ScopedRenderStateDriverStubs driver;
    MG_Backend::DirectGLES::FramebufferImpl::g_alphaWidenedDrawBufferMask = 1u << 1;

    // What the application asked for: write every channel of every draw buffer.
    MG_Impl::GLImpl::ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    MG_Backend::DirectGLES::RenderStateImpl::SyncRenderState(/*forColorClear=*/false);

    // What the driver was told. Draw buffer 0 is untouched; draw buffer 1 loses alpha.
    ASSERT_TRUE(g_driverIndexedColorMasks[0].seen);
    EXPECT_EQ(g_driverIndexedColorMasks[0].r, GL_TRUE);
    EXPECT_EQ(g_driverIndexedColorMasks[0].g, GL_TRUE);
    EXPECT_EQ(g_driverIndexedColorMasks[0].b, GL_TRUE);
    EXPECT_EQ(g_driverIndexedColorMasks[0].a, GL_TRUE);
    ASSERT_TRUE(g_driverIndexedColorMasks[1].seen);
    EXPECT_EQ(g_driverIndexedColorMasks[1].r, GL_TRUE);
    EXPECT_EQ(g_driverIndexedColorMasks[1].g, GL_TRUE);
    EXPECT_EQ(g_driverIndexedColorMasks[1].b, GL_TRUE);
    EXPECT_EQ(g_driverIndexedColorMasks[1].a, GL_FALSE) << "a widened draw buffer must not take alpha writes";

    // And what the application sees back. The doctoring lives entirely on the push; the frontend
    // state it is derived from is never written, so glGet still answers with the app's value.
    GLboolean appMask[4] = {GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE};
    MG_Impl::GLImpl::GetBooleanv(GL_COLOR_WRITEMASK, appMask);
    EXPECT_EQ(appMask[0], GL_TRUE);
    EXPECT_EQ(appMask[1], GL_TRUE);
    EXPECT_EQ(appMask[2], GL_TRUE);
    EXPECT_EQ(appMask[3], GL_TRUE) << "glGet(GL_COLOR_WRITEMASK) must report the application's mask";
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(FramebufferTest, ClearIntoAWidenedDrawBufferKeepsAlphaWritableAndSubstitutesOne) {
    ScopedRenderStateDriverStubs driver;
    MG_Backend::DirectGLES::FramebufferImpl::g_alphaWidenedDrawBufferMask = 1u << 1;
    MG_Impl::GLImpl::ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    // A draw first, so the mask really is doctored when the clear arrives...
    MG_Backend::DirectGLES::RenderStateImpl::SyncRenderState(/*forColorClear=*/false);
    ASSERT_EQ(g_driverIndexedColorMasks[1].a, GL_FALSE);

    // ...and now the clear, with NOTHING changed in the frontend parameter block. The frontend's
    // render-state version has not moved, so only the purpose-aware memo can force this push -
    // without it the clear would inherit the draw's alpha-off mask and never write the 1.0.
    ResetRecordedColorMasks();
    MG_Backend::DirectGLES::RenderStateImpl::SyncRenderState(/*forColorClear=*/true);
    ASSERT_TRUE(g_driverIndexedColorMasks[1].seen) << "the clear must re-push the colour mask";
    EXPECT_EQ(g_driverIndexedColorMasks[1].a, GL_TRUE) << "a clear is what puts the 1.0 in the stored alpha";

    // The value that clear writes: the application's RGB, alpha replaced by the 1.0 the
    // three-channel format implies, and only on the widened buffer.
    const GLfloat appColor[4] = {0.25f, 0.5f, 0.75f, 0.0f};
    GLfloat scratch[4] = {};
    const GLfloat* widened =
        MG_Backend::DirectGLES::FramebufferImpl::SubstituteWidenedClearAlpha(appColor, true, 1.0f, scratch);
    EXPECT_EQ(widened[0], 0.25f);
    EXPECT_EQ(widened[1], 0.5f);
    EXPECT_EQ(widened[2], 0.75f);
    EXPECT_EQ(widened[3], 1.0f);

    const GLfloat* untouched =
        MG_Backend::DirectGLES::FramebufferImpl::SubstituteWidenedClearAlpha(appColor, false, 1.0f, scratch);
    EXPECT_EQ(untouched, appColor) << "a native attachment's clear must not even be copied";

    // An integer widened format (GL_RGB8UI -> GL_RGBA8UI) carries the INTEGER one, not a
    // saturated field: glClearBufferuiv takes the value verbatim.
    const GLuint appIntegerColor[4] = {7u, 8u, 9u, 0u};
    GLuint integerScratch[4] = {};
    const GLuint* widenedInteger = MG_Backend::DirectGLES::FramebufferImpl::SubstituteWidenedClearAlpha(
        appIntegerColor, true, GLuint(1), integerScratch);
    EXPECT_EQ(widenedInteger[3], 1u);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(FramebufferTest, ApplicationAlphaMaskOffIsStillHonouredOnANativeDrawBuffer) {
    // The doctoring only ever REMOVES alpha writes on a widened buffer; it must never add them
    // back on a buffer the application masked itself, and must never touch a native one.
    ScopedRenderStateDriverStubs driver;
    MG_Backend::DirectGLES::FramebufferImpl::g_alphaWidenedDrawBufferMask = 1u << 1;

    MG_Impl::GLImpl::ColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    MG_Impl::GLImpl::ColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    MG_Impl::GLImpl::ColorMaski(2, GL_FALSE, GL_TRUE, GL_FALSE, GL_TRUE);
    MG_Backend::DirectGLES::RenderStateImpl::SyncRenderState(/*forColorClear=*/false);

    EXPECT_EQ(g_driverIndexedColorMasks[0].a, GL_FALSE) << "the application's own alpha mask survives";
    EXPECT_EQ(g_driverIndexedColorMasks[1].a, GL_FALSE) << "the widened buffer loses alpha";
    EXPECT_EQ(g_driverIndexedColorMasks[2].r, GL_FALSE);
    EXPECT_EQ(g_driverIndexedColorMasks[2].g, GL_TRUE);
    EXPECT_EQ(g_driverIndexedColorMasks[2].b, GL_FALSE);
    EXPECT_EQ(g_driverIndexedColorMasks[2].a, GL_TRUE) << "a native buffer keeps its alpha writes";
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// --- Dual-source blending without GL_EXT_blend_func_extended ------------------------------------
//
// GL_SRC1_* blend factors are core GL since 3.3, GLES core has nothing equivalent, and the ES
// driver may or may not carry GL_EXT_blend_func_extended. When it does, the factors translate and
// blend properly - the positive case below. When it does not, the blend block used to
// THROW_EXCEPTION, which is a plain `throw` (MG_Util/Types.h) with no catch anywhere in MG_Impl or
// MG_Backend, so it unwound out through the C GL ABI and killed the process over one unsupported
// blend factor. It now DECLINES: the draw buffer is pushed with blending off and neutral One/Zero
// factors, and the loss is logged once.
//
// Both halves are asserted at the seam that matters - what the ES driver is actually handed -
// because a GL_SRC1_* enum reaching a driver without the extension is the other failure mode: the
// driver answers GL_INVALID_ENUM, keeps whatever factors were set before, and mis-blends silently.

TEST_F(FramebufferTest, DualSourceBlendFactorsReachTheDriverWhenTheExtensionIsThere) {
    ScopedRenderStateDriverStubs driver(/*dualSourceBlendSupported=*/true);

    MG_Impl::GLImpl::Enable(GL_BLEND);
    MG_Impl::GLImpl::BlendFunc(GL_SRC1_COLOR, GL_ONE_MINUS_SRC1_COLOR);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR) << "GL_SRC1_* is core since 3.3; glBlendFunc must take it";
    ResetRecordedBlend();
    MG_Backend::DirectGLES::RenderStateImpl::SyncRenderState(/*forColorClear=*/false);

    ASSERT_TRUE(g_driverBlend[0].factorsSeen);
    EXPECT_TRUE(g_driverBlend[0].enabled) << "nothing may decline a blend the driver can do";
    EXPECT_EQ(g_driverBlend[0].srcRGB, static_cast<GLenum>(GL_SRC1_COLOR));
    EXPECT_EQ(g_driverBlend[0].dstRGB, static_cast<GLenum>(GL_ONE_MINUS_SRC1_COLOR));
    EXPECT_EQ(g_driverBlend[0].srcAlpha, static_cast<GLenum>(GL_SRC1_COLOR));
    EXPECT_EQ(g_driverBlend[0].dstAlpha, static_cast<GLenum>(GL_ONE_MINUS_SRC1_COLOR));
}

TEST_F(FramebufferTest, DualSourceBlendIsDeclinedRatherThanThrownWhenTheExtensionIsMissing) {
    ScopedRenderStateDriverStubs driver(/*dualSourceBlendSupported=*/false);

    MG_Impl::GLImpl::Enable(GL_BLEND);
    MG_Impl::GLImpl::BlendFunc(GL_SRC1_ALPHA, GL_ONE_MINUS_SRC1_ALPHA);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR)
        << "the FRONTEND accepts the factor whatever the driver can do - the decline is a backend decision";
    ResetRecordedBlend();

    // The whole point: this used to be `throw std::runtime_error` straight through the GL ABI.
    ASSERT_NO_THROW(MG_Backend::DirectGLES::RenderStateImpl::SyncRenderState(/*forColorClear=*/false));

    ASSERT_TRUE(g_driverBlend[0].enableSeen) << "the blend enable still has to be pushed";
    EXPECT_FALSE(g_driverBlend[0].enabled) << "a blend the driver cannot do is declined, not attempted";
    for (Uint i = 0; i < kRecordedDrawBuffers; ++i) {
        EXPECT_NE(g_driverBlend[i].srcRGB, static_cast<GLenum>(GL_SRC1_ALPHA))
            << "draw buffer " << i << ": no GL_SRC1_* enum may reach a driver without the extension";
        EXPECT_NE(g_driverBlend[i].dstRGB, static_cast<GLenum>(GL_ONE_MINUS_SRC1_ALPHA)) << "draw buffer " << i;
        EXPECT_NE(g_driverBlend[i].srcAlpha, static_cast<GLenum>(GL_SRC1_ALPHA)) << "draw buffer " << i;
        EXPECT_NE(g_driverBlend[i].dstAlpha, static_cast<GLenum>(GL_ONE_MINUS_SRC1_ALPHA)) << "draw buffer " << i;
    }

    // The decline is scoped to the offending factor, not to blending as a whole: an ordinary
    // blend on the same driver still goes through, and the SAME sync that declined the first one
    // is what has to push it.
    MG_Impl::GLImpl::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    ResetRecordedBlend();
    MG_Backend::DirectGLES::RenderStateImpl::SyncRenderState(/*forColorClear=*/false);
    ASSERT_TRUE(g_driverBlend[0].factorsSeen);
    EXPECT_TRUE(g_driverBlend[0].enabled);
    EXPECT_EQ(g_driverBlend[0].srcRGB, static_cast<GLenum>(GL_SRC_ALPHA));
    EXPECT_EQ(g_driverBlend[0].dstRGB, static_cast<GLenum>(GL_ONE_MINUS_SRC_ALPHA));
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// The half the first version of the decline missed: the FACTOR push is not gated on Enabled, so
// GL_BLEND being OFF does not keep a GL_SRC1_* enum away from a driver that cannot parse it. This
// is the sequence - `glDisable(GL_BLEND); glBlendFunc(GL_SRC1_ALPHA, ...)` then any draw or clear -
// and it needs no dual-source shader at all, which is why it survived both the enabled-path unit
// case above and the integration scenario (that one skips on exactly the extension-less lanes this
// concerns, because its probe needs a dual-source program to render).
//
// What a leaked enum costs: the driver answers GL_INVALID_ENUM and keeps its previous factors, so
// the error sits in the ES context's own queue for the next internal `glGetError() == GL_NO_ERROR`
// probe to read as its own failure, and this backend's shadow records factors the context rejected.
TEST_F(FramebufferTest, DualSourceFactorsAreDeclinedEvenWithBlendingDisabled) {
    ScopedRenderStateDriverStubs driver(/*dualSourceBlendSupported=*/false);

    MG_Impl::GLImpl::Disable(GL_BLEND);
    MG_Impl::GLImpl::BlendFunc(GL_SRC1_ALPHA, GL_ONE_MINUS_SRC1_ALPHA);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    ResetRecordedBlend();

    ASSERT_NO_THROW(MG_Backend::DirectGLES::RenderStateImpl::SyncRenderState(/*forColorClear=*/false));

    for (Uint i = 0; i < kRecordedDrawBuffers; ++i) {
        EXPECT_FALSE(g_driverBlend[i].enabled) << "draw buffer " << i << ": blending was never enabled";
        EXPECT_NE(g_driverBlend[i].srcRGB, static_cast<GLenum>(GL_SRC1_ALPHA))
            << "draw buffer " << i
            << ": a GL_SRC1_* enum must not reach a driver without the extension even with GL_BLEND off";
        EXPECT_NE(g_driverBlend[i].dstRGB, static_cast<GLenum>(GL_ONE_MINUS_SRC1_ALPHA)) << "draw buffer " << i;
        EXPECT_NE(g_driverBlend[i].srcAlpha, static_cast<GLenum>(GL_SRC1_ALPHA)) << "draw buffer " << i;
        EXPECT_NE(g_driverBlend[i].dstAlpha, static_cast<GLenum>(GL_ONE_MINUS_SRC1_ALPHA)) << "draw buffer " << i;
    }

    // A clear reaches the same block by the same route (SyncRenderState(forColorClear=true)), and
    // the flag only steers the alpha-widen colour mask, so it must not reopen this either.
    MG_Impl::GLImpl::BlendFunc(GL_SRC1_COLOR, GL_ONE_MINUS_SRC1_COLOR);
    ResetRecordedBlend();
    ASSERT_NO_THROW(MG_Backend::DirectGLES::RenderStateImpl::SyncRenderState(/*forColorClear=*/true));
    for (Uint i = 0; i < kRecordedDrawBuffers; ++i) {
        EXPECT_NE(g_driverBlend[i].srcRGB, static_cast<GLenum>(GL_SRC1_COLOR)) << "draw buffer " << i;
        EXPECT_NE(g_driverBlend[i].dstRGB, static_cast<GLenum>(GL_ONE_MINUS_SRC1_COLOR)) << "draw buffer " << i;
    }

    // And the shadow records what was PUSHED, not what the frontend holds - otherwise the next
    // switch to an ordinary factor diffs against state the ES context never received.
    MG_Impl::GLImpl::Enable(GL_BLEND);
    MG_Impl::GLImpl::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    ResetRecordedBlend();
    MG_Backend::DirectGLES::RenderStateImpl::SyncRenderState(/*forColorClear=*/false);
    ASSERT_TRUE(g_driverBlend[0].factorsSeen);
    EXPECT_TRUE(g_driverBlend[0].enabled) << "the enable has to be pushed - the shadow said 'off' because it was";
    EXPECT_EQ(g_driverBlend[0].srcRGB, static_cast<GLenum>(GL_SRC_ALPHA));
    EXPECT_EQ(g_driverBlend[0].dstRGB, static_cast<GLenum>(GL_ONE_MINUS_SRC_ALPHA));
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// The capable driver is unaffected by the ungating: GL_BLEND off with SRC1 factors set is a state
// an application may legitimately hold, and the factors still have to reach a driver that parses
// them - otherwise the next glEnable(GL_BLEND) would blend against neutralised state.
TEST_F(FramebufferTest, DualSourceFactorsWithBlendingDisabledStillReachACapableDriver) {
    ScopedRenderStateDriverStubs driver(/*dualSourceBlendSupported=*/true);

    MG_Impl::GLImpl::Disable(GL_BLEND);
    MG_Impl::GLImpl::BlendFunc(GL_SRC1_ALPHA, GL_ONE_MINUS_SRC1_ALPHA);
    ResetRecordedBlend();
    MG_Backend::DirectGLES::RenderStateImpl::SyncRenderState(/*forColorClear=*/false);

    ASSERT_TRUE(g_driverBlend[0].factorsSeen);
    EXPECT_FALSE(g_driverBlend[0].enabled);
    EXPECT_EQ(g_driverBlend[0].srcRGB, static_cast<GLenum>(GL_SRC1_ALPHA));
    EXPECT_EQ(g_driverBlend[0].dstRGB, static_cast<GLenum>(GL_ONE_MINUS_SRC1_ALPHA));
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// --- glFramebufferTexture error conditions (GL 4.6 core 9.2.8) ---------------------------------
//
// Four of them were missing from the bound-target path while its DSA sibling
// (glNamedFramebufferTexture) implemented all four, which is what KHR-GL4x.geometry_shader.
// layered_fbo.fb_texture_* fails on. Two of them - the attachment-range check and the
// default-framebuffer rejection - newly REFUSE calls that used to succeed, so they are pinned
// here rather than left to the conformance suite.

TEST_F(FramebufferTest, FramebufferTextureRejectsTheDefaultFramebuffer) {
    GLuint texture = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &texture);
    MG_Impl::GLImpl::TextureStorage2D(texture, 1, GL_RGBA8, 64, 32);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // MobileGL models framebuffer 0 as a real FramebufferObject, so the null test that used to
    // stand in for this could never fire and the attach silently "succeeded".
    MG_Impl::GLImpl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    MG_Impl::GLImpl::FramebufferTexture(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, texture, 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_OPERATION);
    DrainPendingGlErrors();
}

TEST_F(FramebufferTest, FramebufferTextureRejectsAColourAttachmentPastTheLimit) {
    GLuint framebuffer = 0;
    GLuint texture = 0;
    MG_Impl::GLImpl::CreateFramebuffers(1, &framebuffer);
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &texture);
    MG_Impl::GLImpl::TextureStorage2D(texture, 1, GL_RGBA8, 64, 32);
    MG_Impl::GLImpl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // The same limit ValidateColorAttachmentInRange reads, so the test cannot disagree with the
    // implementation about where the boundary is.
    const GLint limit = MG_Backend::pActiveBackendObject
                            ? static_cast<GLint>(
                                  MG_Backend::pActiveBackendObject->GetDynamicParameters().MaxColorAttachments)
                            : static_cast<GLint>(MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS);
    ASSERT_GT(limit, 0);
    ASSERT_LT(limit, 32) << "the test needs a colour attachment enum past the limit to exist";

    MG_Impl::GLImpl::FramebufferTexture(GL_DRAW_FRAMEBUFFER,
                                        static_cast<GLenum>(GL_COLOR_ATTACHMENT0 + limit), texture, 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_OPERATION);
    DrainPendingGlErrors();

    // The last legal one still attaches, so the boundary is off-by-none.
    MG_Impl::GLImpl::FramebufferTexture(GL_DRAW_FRAMEBUFFER,
                                        static_cast<GLenum>(GL_COLOR_ATTACHMENT0 + limit - 1), texture, 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(FramebufferTest, FramebufferTextureReportsInvalidValueForANameThatWasNeverGenerated) {
    GLuint framebuffer = 0;
    MG_Impl::GLImpl::CreateFramebuffers(1, &framebuffer);
    MG_Impl::GLImpl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // INVALID_VALUE, not INVALID_OPERATION: the entry point used to resolve the texture object
    // first and report the miss with the wrong code, pre-empting ValidateTextureName.
    const GLuint neverGenerated = std::numeric_limits<GLuint>::max();
    ASSERT_FALSE(MG_State::pGLContext->ValidateTextureName(neverGenerated));
    MG_Impl::GLImpl::FramebufferTexture(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, neverGenerated, 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_VALUE);
    DrainPendingGlErrors();
}

TEST_F(FramebufferTest, FramebufferTextureRejectsALevelTheTextureDoesNotHave) {
    GLuint framebuffer = 0;
    GLuint texture = 0;
    MG_Impl::GLImpl::CreateFramebuffers(1, &framebuffer);
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &texture);
    // Two levels of immutable storage: level 1 is legal, level 2 is not.
    MG_Impl::GLImpl::TextureStorage2D(texture, 2, GL_RGBA8, 64, 32);
    MG_Impl::GLImpl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::FramebufferTexture(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, texture, 1);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR) << "the last level the texture has is legal";

    MG_Impl::GLImpl::FramebufferTexture(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, texture, 2);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_VALUE);
    DrainPendingGlErrors();

    MG_Impl::GLImpl::FramebufferTexture(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, texture, -1);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_VALUE);
    DrainPendingGlErrors();
}

// The four conditions above are stated once in GL 4.6 core 9.2.8 for the WHOLE family, and
// glFramebufferTexture2D / 3D / TextureLayer reach the attachment through their own code rather
// than through the shared helper - so each of them has to be asked separately or one entry point
// answers differently from its aliases. glFramebufferTexture2D is the most-used of the five, and
// the default-framebuffer case is the damaging one: the attach used to succeed and replace
// framebuffer 0's colour attachment, which nothing ever puts back.

TEST_F(FramebufferTest, FramebufferTexture2DRejectsTheDefaultFramebufferAndBadAttachments) {
    GLuint texture = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &texture);
    MG_Impl::GLImpl::TextureStorage2D(texture, 2, GL_RGBA8, 64, 32);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    const auto defaultFramebuffer = MG_State::pGLContext->GetFramebufferObject(0);
    ASSERT_NE(defaultFramebuffer, nullptr);
    const auto& colorBefore = defaultFramebuffer->GetAttachment(FramebufferAttachmentType::Color0);
    const Bool hadTextureBefore = colorBefore.IsTexture();

    MG_Impl::GLImpl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    MG_Impl::GLImpl::FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_OPERATION);
    DrainPendingGlErrors();
    // ...and, more to the point, the default framebuffer still describes the surface.
    const auto& colorAfter = defaultFramebuffer->GetAttachment(FramebufferAttachmentType::Color0);
    EXPECT_EQ(colorAfter.IsTexture(), hadTextureBefore);
    if (colorAfter.IsTexture() && hadTextureBefore) {
        EXPECT_NE(colorAfter.GetTexture()->GetExternalIndex(), texture)
            << "the refused attach must not have replaced framebuffer 0's colour attachment";
    }

    GLuint framebuffer = 0;
    MG_Impl::GLImpl::CreateFramebuffers(1, &framebuffer);
    MG_Impl::GLImpl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    const GLint limit = MG_Backend::pActiveBackendObject
                            ? static_cast<GLint>(
                                  MG_Backend::pActiveBackendObject->GetDynamicParameters().MaxColorAttachments)
                            : static_cast<GLint>(MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS);
    MG_Impl::GLImpl::FramebufferTexture2D(GL_DRAW_FRAMEBUFFER,
                                          static_cast<GLenum>(GL_COLOR_ATTACHMENT0 + limit), GL_TEXTURE_2D,
                                          texture, 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_OPERATION);
    DrainPendingGlErrors();

    MG_Impl::GLImpl::FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 2);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_VALUE) << "the texture has two levels, not three";
    DrainPendingGlErrors();

    MG_Impl::GLImpl::FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, -1);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_VALUE);
    DrainPendingGlErrors();

    // The legal call still works, so the boundary is off-by-none.
    MG_Impl::GLImpl::FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 1);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(FramebufferTest, FramebufferTextureLayerRejectsTheDefaultFramebufferAndBadLevels) {
    GLuint texture = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D_ARRAY, 1, &texture);
    MG_Impl::GLImpl::TextureStorage3D(texture, 2, GL_RGBA8, 16, 16, 4);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // The attach path used to bypass every one of these while the DETACH path (texture == 0) went
    // through the fixed helper, so one entry point answered two different ways.
    MG_Impl::GLImpl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    MG_Impl::GLImpl::FramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, texture, 0, 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_OPERATION);
    DrainPendingGlErrors();

    GLuint framebuffer = 0;
    MG_Impl::GLImpl::CreateFramebuffers(1, &framebuffer);
    MG_Impl::GLImpl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::FramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, texture, 2, 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_VALUE);
    DrainPendingGlErrors();

    MG_Impl::GLImpl::FramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, texture, 1, 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// The DSA sibling is the entry point the bound-target family was aligned WITH, so an out-of-range
// immutable level has to be rejected there too - otherwise the alignment created a fresh
// asymmetry in the opposite direction.
TEST_F(FramebufferTest, NamedFramebufferTextureRejectsALevelTheTextureDoesNotHave) {
    GLuint framebuffer = 0;
    GLuint texture = 0;
    MG_Impl::GLImpl::CreateFramebuffers(1, &framebuffer);
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &texture);
    MG_Impl::GLImpl::TextureStorage2D(texture, 2, GL_RGBA8, 64, 32);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::NamedFramebufferTexture(framebuffer, GL_COLOR_ATTACHMENT0, texture, 1);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::NamedFramebufferTexture(framebuffer, GL_COLOR_ATTACHMENT0, texture, 2);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_VALUE);
    DrainPendingGlErrors();
}
