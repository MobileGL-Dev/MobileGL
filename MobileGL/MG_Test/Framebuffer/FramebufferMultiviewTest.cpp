// MobileGL - MobileGL/MG_Test/Framebuffer/FramebufferMultiviewTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// GL_OVR_multiview on the frontend: the attach errors, both completeness rules, the attachment
// queries, GL_MAX_VIEWS_OVR and the read-side refusal, against a backend whose GL_MAX_VIEWS_OVR the
// test chooses. None of it reaches a driver.
#include <gtest/gtest.h>

#include "Includes.h"
#include "Init.h"
#include <MG_Backend/BackendObjects.h>
#include <MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.h>
#include <MG_Impl/GLImpl/Getter/GL_Getter.h>
#include <MG_Impl/GLImpl/Texture/GL_Texture.h>
#include <MG_State/GLState/Core.h>

using namespace MobileGL;

namespace {
    class MultiviewLimitsBackend final : public MG_Backend::BackendObject {
    public:
        explicit MultiviewLimitsBackend(Int maxViews) {
            m_params.MaxViews = maxViews;
            m_params.MaxArrayTextureLayers = 8;
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
        const MG_Backend::DynamicBackendParameters& GetDynamicParameters() const override { return m_params; }
        BackendType GetBackendType() const override { return BackendType::Unknown; }

    private:
        MG_Backend::DynamicBackendParameters m_params{};
    };

    class ScopedMultiviewBackend {
    public:
        explicit ScopedMultiviewBackend(Int maxViews) : m_previous(Move(MG_Backend::pActiveBackendObject)) {
            MG_Backend::pActiveBackendObject = MakeUnique<MultiviewLimitsBackend>(maxViews);
        }
        ~ScopedMultiviewBackend() { MG_Backend::pActiveBackendObject = Move(m_previous); }

    private:
        UniquePtr<MG_Backend::BackendObject> m_previous;
    };

    GLuint MakeArrayTexture(GLsizei layers) {
        GLuint texture = 0;
        MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D_ARRAY, 1, &texture);
        MG_Impl::GLImpl::TextureStorage3D(texture, 1, GL_RGBA8, 4, 4, layers);
        return texture;
    }

    GLint AttachmentParameter(GLuint framebuffer, GLenum attachment, GLenum pname) {
        GLint value = -1;
        MG_Impl::GLImpl::GetNamedFramebufferAttachmentParameteriv(framebuffer, attachment, pname, &value);
        return value;
    }
} // namespace

class FramebufferMultiviewTest : public ::testing::Test {
protected:
    void SetUp() override {
        MobileGL::Initialize();
        for (Int drained = 0; drained < 16 && MG_Impl::GLImpl::GetError() != GL_NO_ERROR; ++drained) {}
        m_defaultFramebuffer = MG_State::pGLContext->GetFramebufferObject(0);
        ASSERT_NE(m_defaultFramebuffer, nullptr);
    }

    void TearDown() override {
        MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read).Bind(m_defaultFramebuffer);
        MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).Bind(m_defaultFramebuffer);
        EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR) << "test left an unconsumed GL error behind";
    }

    static void ExpectSingleGlError(GLenum expected) {
        EXPECT_EQ(MG_Impl::GLImpl::GetError(), expected);
        EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR) << "the call recorded more than one error";
    }

    SharedPtr<MG_State::GLState::FramebufferObject> m_defaultFramebuffer;
};

TEST_F(FramebufferMultiviewTest, WithoutBackendMultiviewTheEntryPointsAndTokensAreRefused) {
    ScopedMultiviewBackend backend(0);
    GLuint framebuffer = 0;
    MG_Impl::GLImpl::CreateFramebuffers(1, &framebuffer);
    const GLuint texture = MakeArrayTexture(2);

    MG_Impl::GLImpl::NamedFramebufferTextureMultiviewOVR(framebuffer, GL_COLOR_ATTACHMENT0, texture, 0, 0, 2);
    ExpectSingleGlError(GL_INVALID_OPERATION);
    EXPECT_FALSE(MG_State::pGLContext->GetFramebufferObject(framebuffer)
                     ->GetAttachment(FramebufferAttachmentType::Color0)
                     .IsTexture());

    GLint maxViews = -1;
    MG_Impl::GLImpl::GetIntegerv(GL_MAX_VIEWS_OVR, &maxViews);
    ExpectSingleGlError(GL_INVALID_ENUM);

    MG_Impl::GLImpl::NamedFramebufferTextureLayer(framebuffer, GL_COLOR_ATTACHMENT0, texture, 0, 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    AttachmentParameter(framebuffer, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_NUM_VIEWS_OVR);
    ExpectSingleGlError(GL_INVALID_ENUM);
}

TEST_F(FramebufferMultiviewTest, AttachesViewsOfAnArrayTextureAndAnswersTheQueries) {
    ScopedMultiviewBackend backend(4);
    GLint maxViews = -1;
    MG_Impl::GLImpl::GetIntegerv(GL_MAX_VIEWS_OVR, &maxViews);
    EXPECT_EQ(maxViews, 4);

    GLuint framebuffer = 0;
    MG_Impl::GLImpl::CreateFramebuffers(1, &framebuffer);
    const GLuint texture = MakeArrayTexture(4);
    MG_Impl::GLImpl::NamedFramebufferTextureMultiviewOVR(framebuffer, GL_COLOR_ATTACHMENT0, texture, 0, 1, 2);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    const auto framebufferObject = MG_State::pGLContext->GetFramebufferObject(framebuffer);
    const auto& attachment = framebufferObject->GetAttachment(FramebufferAttachmentType::Color0);
    EXPECT_TRUE(attachment.IsMultiview());
    EXPECT_EQ(attachment.GetNumViews(), 2);
    EXPECT_FALSE(attachment.IsLayered());
    EXPECT_EQ(framebufferObject->GetViewCount(), 2);

    EXPECT_EQ(AttachmentParameter(framebuffer, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_NUM_VIEWS_OVR),
              2);
    EXPECT_EQ(
        AttachmentParameter(framebuffer, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_BASE_VIEW_INDEX_OVR),
        1);
    EXPECT_EQ(AttachmentParameter(framebuffer, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER), 1);
    EXPECT_EQ(MG_Impl::GLImpl::CheckNamedFramebufferStatus(framebuffer, GL_DRAW_FRAMEBUFFER),
              static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));

    // A plain layer attachment is not multiview, and says so. Layer 0, the one every backend backs.
    MG_Impl::GLImpl::NamedFramebufferTextureLayer(framebuffer, GL_COLOR_ATTACHMENT0, texture, 0, 0);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_EQ(AttachmentParameter(framebuffer, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_NUM_VIEWS_OVR),
              0);
    EXPECT_EQ(framebufferObject->GetViewCount(), 1);

    // Texture 0 detaches.
    MG_Impl::GLImpl::NamedFramebufferTextureMultiviewOVR(framebuffer, GL_COLOR_ATTACHMENT0, 0, 0, 0, 2);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_FALSE(framebufferObject->GetAttachment(FramebufferAttachmentType::Color0).IsTexture());
}

TEST_F(FramebufferMultiviewTest, AttachRefusesWhatTheSpecRefuses) {
    ScopedMultiviewBackend backend(4);
    GLuint framebuffer = 0;
    MG_Impl::GLImpl::CreateFramebuffers(1, &framebuffer);
    const GLuint arrayTexture = MakeArrayTexture(8);
    GLuint texture2D = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &texture2D);
    MG_Impl::GLImpl::TextureStorage2D(texture2D, 1, GL_RGBA8, 4, 4);

    MG_Impl::GLImpl::NamedFramebufferTextureMultiviewOVR(framebuffer, GL_COLOR_ATTACHMENT0, arrayTexture, 0, 0, 0);
    ExpectSingleGlError(GL_INVALID_VALUE);
    MG_Impl::GLImpl::NamedFramebufferTextureMultiviewOVR(framebuffer, GL_COLOR_ATTACHMENT0, arrayTexture, 0, 0, 5);
    ExpectSingleGlError(GL_INVALID_VALUE);
    // The view count is checked even when detaching.
    MG_Impl::GLImpl::NamedFramebufferTextureMultiviewOVR(framebuffer, GL_COLOR_ATTACHMENT0, 0, 0, 0, 0);
    ExpectSingleGlError(GL_INVALID_VALUE);
    MG_Impl::GLImpl::NamedFramebufferTextureMultiviewOVR(framebuffer, GL_COLOR_ATTACHMENT0, texture2D, 0, 0, 2);
    ExpectSingleGlError(GL_INVALID_OPERATION);
    MG_Impl::GLImpl::NamedFramebufferTextureMultiviewOVR(framebuffer, GL_COLOR_ATTACHMENT0, arrayTexture, 0, -1, 2);
    ExpectSingleGlError(GL_INVALID_VALUE);
    // GL_MAX_ARRAY_TEXTURE_LAYERS is 8: views 7 and 8 do not both exist.
    MG_Impl::GLImpl::NamedFramebufferTextureMultiviewOVR(framebuffer, GL_COLOR_ATTACHMENT0, arrayTexture, 0, 7, 2);
    ExpectSingleGlError(GL_INVALID_VALUE);
    MG_Impl::GLImpl::NamedFramebufferTextureMultiviewOVR(framebuffer, GL_COLOR_ATTACHMENT0, arrayTexture, 0, 6, 2);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // The default framebuffer has no attachment points to name.
    MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).Bind(m_defaultFramebuffer);
    MG_Impl::GLImpl::FramebufferTextureMultiviewOVR(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, arrayTexture, 0, 0, 2);
    ExpectSingleGlError(GL_INVALID_OPERATION);
}

TEST_F(FramebufferMultiviewTest, CompletenessNeedsEveryViewAndOneViewCount) {
    ScopedMultiviewBackend backend(4);
    const GLuint fourLayers = MakeArrayTexture(4);
    const GLuint otherFourLayers = MakeArrayTexture(4);

    GLuint pastTheEnd = 0;
    MG_Impl::GLImpl::CreateFramebuffers(1, &pastTheEnd);
    MG_Impl::GLImpl::NamedFramebufferTextureMultiviewOVR(pastTheEnd, GL_COLOR_ATTACHMENT0, fourLayers, 0, 3, 2);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR) << "the layer count is a completeness question";
    EXPECT_EQ(MG_Impl::GLImpl::CheckNamedFramebufferStatus(pastTheEnd, GL_DRAW_FRAMEBUFFER),
              static_cast<GLenum>(GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT));

    GLuint mixed = 0;
    MG_Impl::GLImpl::CreateFramebuffers(1, &mixed);
    MG_Impl::GLImpl::NamedFramebufferTextureMultiviewOVR(mixed, GL_COLOR_ATTACHMENT0, fourLayers, 0, 0, 2);
    MG_Impl::GLImpl::NamedFramebufferTextureLayer(mixed, GL_COLOR_ATTACHMENT1, otherFourLayers, 0, 0);
    EXPECT_EQ(MG_Impl::GLImpl::CheckNamedFramebufferStatus(mixed, GL_DRAW_FRAMEBUFFER),
              static_cast<GLenum>(GL_FRAMEBUFFER_INCOMPLETE_VIEW_TARGETS_OVR));

    GLuint differentCounts = 0;
    MG_Impl::GLImpl::CreateFramebuffers(1, &differentCounts);
    MG_Impl::GLImpl::NamedFramebufferTextureMultiviewOVR(differentCounts, GL_COLOR_ATTACHMENT0, fourLayers, 0, 0, 2);
    MG_Impl::GLImpl::NamedFramebufferTextureMultiviewOVR(differentCounts, GL_COLOR_ATTACHMENT1, otherFourLayers, 0, 0,
                                                         3);
    EXPECT_EQ(MG_Impl::GLImpl::CheckNamedFramebufferStatus(differentCounts, GL_DRAW_FRAMEBUFFER),
              static_cast<GLenum>(GL_FRAMEBUFFER_INCOMPLETE_VIEW_TARGETS_OVR));

    // Matching counts with different base views are fine.
    GLuint matching = 0;
    MG_Impl::GLImpl::CreateFramebuffers(1, &matching);
    MG_Impl::GLImpl::NamedFramebufferTextureMultiviewOVR(matching, GL_COLOR_ATTACHMENT0, fourLayers, 0, 0, 2);
    MG_Impl::GLImpl::NamedFramebufferTextureMultiviewOVR(matching, GL_COLOR_ATTACHMENT1, otherFourLayers, 0, 2, 2);
    EXPECT_EQ(MG_Impl::GLImpl::CheckNamedFramebufferStatus(matching, GL_DRAW_FRAMEBUFFER),
              static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(FramebufferMultiviewTest, AReadFramebufferWithSeveralViewsCannotBeRead) {
    ScopedMultiviewBackend backend(4);
    GLuint framebuffer = 0;
    MG_Impl::GLImpl::CreateFramebuffers(1, &framebuffer);
    MG_Impl::GLImpl::NamedFramebufferTextureMultiviewOVR(framebuffer, GL_COLOR_ATTACHMENT0, MakeArrayTexture(2), 0, 0,
                                                         2);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read)
        .Bind(MG_State::pGLContext->GetFramebufferObject(framebuffer));

    Uint8 pixels[4 * 4 * 4] = {};
    MG_Impl::GLImpl::ReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    ExpectSingleGlError(GL_INVALID_FRAMEBUFFER_OPERATION);
    MG_Impl::GLImpl::BlitFramebuffer(0, 0, 4, 4, 0, 0, 4, 4, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    ExpectSingleGlError(GL_INVALID_FRAMEBUFFER_OPERATION);
}
