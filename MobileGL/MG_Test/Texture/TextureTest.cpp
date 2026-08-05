// MobileGL - MobileGL/MG_Test/Texture/TextureTest.cpp
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
#include <Config.h>
#include <MG_Backend/BackendObjects.h>
#include <MG_Backend/DirectGLES/Managers.h>
#include <MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.h>
#include <MG_Impl/GLImpl/Getter/GL_Getter.h>
#include <MG_Impl/GLImpl/RenderState/GL_RenderState.h>
#include <MG_Impl/GLImpl/Sampler/GL_Sampler.h>
#include <MG_Impl/GLImpl/Texture/GL_Texture.h>
#include <MG_State/EGLState/Core.h>
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/TextureState/TextureObject.h>
#include <MG_State/GLState/TextureState/TextureObject2D.h>
#include <MG_Util/Converters/GLToMG/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToGL/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToMG/TextureEnumConverter.h>
#include <MG_Util/Math/SmallFloat.h>
#include <MG_Util/Texture/PixelStoreProcessor.h>
#include <MG_Util/Texture/TextureFormatProcessor.h>
#include <cstring>

using namespace MobileGL;

class TextureTest : public ::testing::Test {
protected:
    // GL error flags are sticky per error code and the context outlives an individual test in this
    // binary, so anything an earlier test left pending would be handed to the next GetError() call -
    // which silently turns error-code assertions into reads of someone else's error. Bounded because
    // there is one flag per code; a runaway would otherwise hang the suite.
    static void DrainPendingGlErrors() {
        for (Int drained = 0; drained < 16 && MG_Impl::GLImpl::GetError() != GL_NO_ERROR; ++drained) {
        }
    }

    // The call under test must raise exactly the expected error and nothing more: a second pending
    // error means one entry point queued several (e.g. a shared validator firing before the
    // specific check), which GetError() would hand out at unrelated call sites later on.
    static void ExpectSingleGlError(GLenum expected) {
        EXPECT_EQ(MG_Impl::GLImpl::GetError(), expected);
        EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR) << "the call recorded more than one error";
    }

    void SetUp() override {
        MobileGL::Initialize();
        DrainPendingGlErrors();
    }

    void TearDown() override {
        // Attribute a leaked error to the test that caused it instead of to whoever runs next.
        EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR) << "test left an unconsumed GL error behind";
    }
};

namespace {
    class FormatCapabilityBackend final : public MobileGL::MG_Backend::BackendObject {
    public:
        FormatCapabilityBackend() {
            auto& cache = MutableFormatCapabilities();
            const auto texture2DIndex =
                MobileGL::MG_Backend::GetFormatCapabilityTargetIndex(TextureTarget::Texture2D);
            const auto texture3DIndex =
                MobileGL::MG_Backend::GetFormatCapabilityTargetIndex(TextureTarget::Texture3D);
            const auto renderbufferIndex =
                MobileGL::MG_Backend::GetRenderbufferFormatCapabilityTargetIndex();
            const auto rgba8Index = static_cast<SizeT>(TextureInternalFormat::RGBA8);
            const auto rg8Index = static_cast<SizeT>(TextureInternalFormat::RG8);
            const auto depthStencilIndex = static_cast<SizeT>(TextureInternalFormat::Depth24Stencil8);

            cache.FullCaps[texture2DIndex][rgba8Index] |= MobileGL::MG_Backend::FormatCapability::Creatable;
            cache.FullCaps[texture2DIndex][rgba8Index] |= MobileGL::MG_Backend::FormatCapability::Sampled;
            cache.FullCaps[texture2DIndex][rgba8Index] |= MobileGL::MG_Backend::FormatCapability::LinearFilter;
            cache.FullCaps[texture2DIndex][rgba8Index] |= MobileGL::MG_Backend::FormatCapability::GenerateMipmap;
            cache.FullCaps[texture2DIndex][rgba8Index] |=
                MobileGL::MG_Backend::FormatCapability::FramebufferRenderable;
            cache.FullCaps[texture2DIndex][rgba8Index] |= MobileGL::MG_Backend::FormatCapability::ColorAttachment;
            cache.SampleCounts[texture2DIndex][rgba8Index] = {1};

            cache.CaveatCaps[texture2DIndex][rg8Index] |= MobileGL::MG_Backend::FormatCapability::Creatable;
            cache.CaveatCaps[texture2DIndex][rg8Index] |= MobileGL::MG_Backend::FormatCapability::Sampled;
            cache.CaveatCaps[texture2DIndex][rg8Index] |= MobileGL::MG_Backend::FormatCapability::LinearFilter;

            cache.FullCaps[texture3DIndex][depthStencilIndex] |=
                MobileGL::MG_Backend::FormatCapability::Creatable;
            cache.FullCaps[texture3DIndex][depthStencilIndex] |=
                MobileGL::MG_Backend::FormatCapability::FramebufferRenderable;
            cache.FullCaps[texture3DIndex][depthStencilIndex] |=
                MobileGL::MG_Backend::FormatCapability::FramebufferLayered;
            cache.FullCaps[texture3DIndex][depthStencilIndex] |=
                MobileGL::MG_Backend::FormatCapability::DepthAttachment;
            cache.FullCaps[texture3DIndex][depthStencilIndex] |=
                MobileGL::MG_Backend::FormatCapability::StencilAttachment;

            cache.FullCaps[renderbufferIndex][depthStencilIndex] |=
                MobileGL::MG_Backend::FormatCapability::Creatable;
            cache.FullCaps[renderbufferIndex][depthStencilIndex] |=
                MobileGL::MG_Backend::FormatCapability::FramebufferRenderable;
            cache.FullCaps[renderbufferIndex][depthStencilIndex] |=
                MobileGL::MG_Backend::FormatCapability::DepthAttachment;
            cache.FullCaps[renderbufferIndex][depthStencilIndex] |=
                MobileGL::MG_Backend::FormatCapability::StencilAttachment;
            cache.FullCaps[renderbufferIndex][depthStencilIndex] |=
                MobileGL::MG_Backend::FormatCapability::MultisampleRenderbuffer;
            cache.SampleCounts[renderbufferIndex][depthStencilIndex] = {4, 2, 1};
        }

        void Initialize() override {}
        Bool InitCapabilities() override { return true; }
        Bool InitWindowSurface() override { return true; }
        const RendererInfo& GetRendererInfo() const override {
            static RendererInfo info = {};
            return info;
        }
        String GetBackendAPIVersionString() const override { return {}; }
        const MobileGL::MG_Backend::GlobalBackendFunctionsTable& GetBackendFunctions() const override {
            static MobileGL::MG_Backend::GlobalBackendFunctionsTable table = {};
            return table;
        }
        const MobileGL::MG_Backend::DynamicBackendParameters& GetDynamicParameters() const override {
            return MutableDynamicParameters();
        }
        // Lets a test stand in a backend limit (e.g. GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT).
        static MobileGL::MG_Backend::DynamicBackendParameters& MutableDynamicParameters() {
            static MobileGL::MG_Backend::DynamicBackendParameters params = {};
            return params;
        }
        BackendType GetBackendType() const override { return BackendType::Unknown; }
    };

    class ScopedBackendOverride {
    public:
        explicit ScopedBackendOverride(UniquePtr<MobileGL::MG_Backend::BackendObject> backend):
            m_previous(Move(MobileGL::MG_Backend::pActiveBackendObject)) {
            MobileGL::MG_Backend::pActiveBackendObject = Move(backend);
        }

        ~ScopedBackendOverride() {
            MobileGL::MG_Backend::pActiveBackendObject = Move(m_previous);
        }

    private:
        UniquePtr<MobileGL::MG_Backend::BackendObject> m_previous;
    };

    const Uint8* GetBoundTexture2DLevelBytes(GLuint texture, Uint level = 0) {
        const auto textureObject = MG_State::pGLContext->GetTextureObject(texture);
        auto* mipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
        return static_cast<const Uint8*>(mipmapObject->MapMipmapData(TextureUploadTarget::Texture2D, level));
    }

    class ScopedTextureBackendFunctionsOverride {
    public:
        ScopedTextureBackendFunctionsOverride(): m_snapshot(MG_Backend::gBackendFunctionsTable) {}
        ~ScopedTextureBackendFunctionsOverride() { MG_Backend::gBackendFunctionsTable = m_snapshot; }

    private:
        MG_Backend::GlobalBackendFunctionsTable m_snapshot;
    };

    struct CopyTexSubImage2DCall {
        Bool Called = false;
        GLenum Target = GL_NONE;
        GLint Level = -1;
        GLint XOffset = -1;
        GLint YOffset = -1;
        GLint X = -1;
        GLint Y = -1;
        GLsizei Width = -1;
        GLsizei Height = -1;
        GLuint BoundTexture = 0;
    } g_copyTexSubImage2DCall;

    void RecordCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y,
                                 GLsizei width, GLsizei height) {
        g_copyTexSubImage2DCall = {
            true, target, level, xoffset, yoffset, x, y, width, height,
            MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit())
                .GetBindingSlot(TextureTarget::Texture2D)
                .GetBoundObject()
                ->GetExternalIndex(),
        };
    }
} // namespace

TEST_F(TextureTest, CreateTexturesCreatesObjectsWithoutBinding) {
    auto& unit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
    const auto boundBefore = unit.GetBindingSlot(TextureTarget::Texture2D).GetBoundObject();

    GLuint texture = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &texture);

    EXPECT_NE(texture, 0u);
    EXPECT_TRUE(MG_State::pGLContext->ValidateTextureObject(texture));

    // CreateTextures must not disturb the unit's binding (which is never empty anymore: at
    // minimum it holds the target's default texture object).
    EXPECT_EQ(unit.GetBindingSlot(TextureTarget::Texture2D).GetBoundObject(), boundBefore);
    EXPECT_NE(unit.GetBindingSlot(TextureTarget::Texture2D).GetBoundObject(),
              MG_State::pGLContext->GetTextureObject(texture));
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, ClearTexImageNullClearsWholeNamedTextureAndMarksStorageDirty) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);

    const Uint8 initialPixels[] = {
        1, 2, 3, 4,
        5, 6, 7, 8,
        9, 10, 11, 12,
        13, 14, 15, 16,
    };
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0,
                                GL_RGBA, GL_UNSIGNED_BYTE, initialPixels);

    const auto textureObject = MG_State::pGLContext->GetTextureObject(texture);
    auto* mipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
    mipmapObject->MarkStorageDirty(TextureUploadTarget::Texture2D, 0, false);

    MG_Impl::GLImpl::ClearTexImage(texture, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    const Uint8* stored = GetBoundTexture2DLevelBytes(texture);
    ASSERT_NE(stored, nullptr);
    const Uint8 zeros[sizeof(initialPixels)] = {};
    EXPECT_EQ(std::memcmp(stored, zeros, sizeof(zeros)), 0);
    EXPECT_TRUE(mipmapObject->IsStorageDirty(TextureUploadTarget::Texture2D, 0));
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, ClearTexImageRepeatsConvertedClearPixel) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0,
                                GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    const Uint8 clearPixel[] = {17, 34, 51, 68};
    MG_Impl::GLImpl::ClearTexImage(texture, 0, GL_RGBA, GL_UNSIGNED_BYTE, clearPixel);

    const Uint8* stored = GetBoundTexture2DLevelBytes(texture);
    ASSERT_NE(stored, nullptr);
    const Uint8 expected[] = {
        17, 34, 51, 68,
        17, 34, 51, 68,
        17, 34, 51, 68,
        17, 34, 51, 68,
    };
    EXPECT_EQ(std::memcmp(stored, expected, sizeof(expected)), 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, ClearTexSubImageClearsOnlyRequestedRectangle) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
    Uint8 initialPixels[3 * 2 * 4];
    std::memset(initialPixels, 0x7f, sizeof(initialPixels));
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 3, 2, 0,
                                GL_RGBA, GL_UNSIGNED_BYTE, initialPixels);

    MG_Impl::GLImpl::ClearTexSubImage(texture, 0, 1, 0, 0, 1, 2, 1,
                                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    const Uint8* stored = GetBoundTexture2DLevelBytes(texture);
    ASSERT_NE(stored, nullptr);
    for (Int y = 0; y < 2; ++y) {
        for (Int x = 0; x < 3; ++x) {
            for (Int channel = 0; channel < 4; ++channel) {
                EXPECT_EQ(stored[(y * 3 + x) * 4 + channel], x == 1 ? 0 : 0x7f);
            }
        }
    }
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, CopyTextureSubImage2DUsesNamedObjectAndRestoresBinding) {
    const ScopedTextureBackendFunctionsOverride backendGuard;
    MG_Backend::gBackendFunctionsTable.GL.CopyTexSubImage2D = RecordCopyTexSubImage2D;
    g_copyTexSubImage2DCall = {};

    GLuint namedTexture = 0;
    GLuint boundTexture = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &namedTexture);
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &boundTexture);
    MG_Impl::GLImpl::BindTextureUnit(0, boundTexture);

    // The copy is only allowed to reach the backend when the destination region fits the level and
    // the read framebuffer can supply pixels, so the call has to be set up as a legal one.
    MG_Impl::GLImpl::TextureStorage2D(namedTexture, 3, GL_RGBA8, 16, 16);

    GLuint readFramebuffer = 0;
    GLuint readTexture = 0;
    MG_Impl::GLImpl::CreateFramebuffers(1, &readFramebuffer);
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &readTexture);
    MG_Impl::GLImpl::TextureStorage2D(readTexture, 1, GL_RGBA8, 16, 16);
    MG_Impl::GLImpl::NamedFramebufferTexture(readFramebuffer, GL_COLOR_ATTACHMENT0, readTexture, 0);
    MG_Impl::GLImpl::BindFramebuffer(GL_READ_FRAMEBUFFER, readFramebuffer);

    const auto boundBefore = MG_State::pGLContext->GetTextureUnitObject(0)
                                 .GetBindingSlot(TextureTarget::Texture2D)
                                 .GetBoundObject();
    MG_Impl::GLImpl::CopyTextureSubImage2D(namedTexture, 2, 3, 4, 5, 6, 7, 8);

    EXPECT_TRUE(g_copyTexSubImage2DCall.Called);
    EXPECT_EQ(g_copyTexSubImage2DCall.Target, GL_TEXTURE_2D);
    EXPECT_EQ(g_copyTexSubImage2DCall.Level, 2);
    EXPECT_EQ(g_copyTexSubImage2DCall.XOffset, 3);
    EXPECT_EQ(g_copyTexSubImage2DCall.YOffset, 4);
    EXPECT_EQ(g_copyTexSubImage2DCall.X, 5);
    EXPECT_EQ(g_copyTexSubImage2DCall.Y, 6);
    EXPECT_EQ(g_copyTexSubImage2DCall.Width, 7);
    EXPECT_EQ(g_copyTexSubImage2DCall.Height, 8);
    EXPECT_EQ(g_copyTexSubImage2DCall.BoundTexture, namedTexture);
    EXPECT_EQ(MG_State::pGLContext->GetTextureUnitObject(0)
                  .GetBindingSlot(TextureTarget::Texture2D)
                  .GetBoundObject(),
              boundBefore);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, CopyTextureSubImage2DRejectsCubeMapTargets) {
    const ScopedTextureBackendFunctionsOverride backendGuard;
    MG_Backend::gBackendFunctionsTable.GL.CopyTexSubImage2D = RecordCopyTexSubImage2D;
    g_copyTexSubImage2DCall = {};

    GLuint cubeTexture = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_CUBE_MAP, 1, &cubeTexture);
    MG_Impl::GLImpl::CopyTextureSubImage2D(cubeTexture, 0, 0, 0, 0, 0, 1, 1);

    // GL 4.6 sec. 8.8: the 2D form only accepts 2D/1D-array/rectangle effective targets.
    EXPECT_FALSE(g_copyTexSubImage2DCall.Called);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
}

TEST_F(TextureTest, ClearTexImageErrorContracts) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0,
                                GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    // Zero texture name is INVALID_OPERATION (ARB_clear_texture).
    MG_Impl::GLImpl::ClearTexImage(0, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), static_cast<GLenum>(GL_INVALID_OPERATION));

    // A negative level is INVALID_VALUE...
    MG_Impl::GLImpl::ClearTexImage(texture, -1, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), static_cast<GLenum>(GL_INVALID_VALUE));

    // ...but clearing a level that was never defined is INVALID_OPERATION.
    MG_Impl::GLImpl::ClearTexImage(texture, 5, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), static_cast<GLenum>(GL_INVALID_OPERATION));

    // A clear region outside the level is INVALID_VALUE.
    MG_Impl::GLImpl::ClearTexSubImage(texture, 0, 1, 1, 0, 4, 4, 1,
                                      GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), static_cast<GLenum>(GL_INVALID_VALUE));

    // An invalid pixel-transfer format is INVALID_ENUM from the shared validators.
    MG_Impl::GLImpl::ClearTexImage(texture, 0, GL_NONE, GL_UNSIGNED_BYTE, nullptr);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), static_cast<GLenum>(GL_INVALID_ENUM));
}

// GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT is float state that must answer every numeric query: GetFloatv
// is authoritative and GetIntegerv would otherwise fall through to its INVALID_ENUM default.
TEST_F(TextureTest, MaxTextureMaxAnisotropyIsAnsweredFromTheBackendLimit) {
    auto backend = MakeUnique<FormatCapabilityBackend>();
    FormatCapabilityBackend::MutableDynamicParameters().MaxTextureMaxAnisotropy = 16.0f;
    ScopedBackendOverride override(Move(backend));

    GLfloat floatValue = 0.0f;
    MG_Impl::GLImpl::GetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &floatValue);
    EXPECT_FLOAT_EQ(floatValue, 16.0f);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    GLint integerValue = 0;
    MG_Impl::GLImpl::GetIntegerv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &integerValue);
    EXPECT_EQ(integerValue, 16);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // A backend without anisotropy reports the no-anisotropy floor rather than erroring.
    FormatCapabilityBackend::MutableDynamicParameters().MaxTextureMaxAnisotropy = 1.0f;
    MG_Impl::GLImpl::GetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &floatValue);
    EXPECT_FLOAT_EQ(floatValue, 1.0f);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, TextureMaxAnisotropyDefaultsToOneAndRoundTripsWithoutRedundantVersionBumps) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    const auto textureObject = MG_State::pGLContext->GetTextureObject(texture);
    ASSERT_NE(textureObject, nullptr);
    const auto& samplerObject = textureObject->GetSamplerObject();
    ASSERT_NE(samplerObject, nullptr);

    GLfloat floatValue = 0.0f;
    MG_Impl::GLImpl::GetTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, &floatValue);
    EXPECT_FLOAT_EQ(floatValue, 1.0f);
    EXPECT_FLOAT_EQ(samplerObject->GetMaxAnisotropy(), 1.0f);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    const Uint16 initialVersion = samplerObject->GetVersion();
    MG_Impl::GLImpl::TexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, 4.0f);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_FLOAT_EQ(samplerObject->GetMaxAnisotropy(), 4.0f);
    EXPECT_EQ(samplerObject->GetVersion(), static_cast<Uint16>(initialVersion + 1));

    GLint integerValue = 0;
    MG_Impl::GLImpl::GetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, &integerValue);
    EXPECT_EQ(integerValue, 4);
    MG_Impl::GLImpl::GetTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, &floatValue);
    EXPECT_FLOAT_EQ(floatValue, 4.0f);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    const Uint16 setVersion = samplerObject->GetVersion();
    MG_Impl::GLImpl::TexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, 4.0f);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_EQ(samplerObject->GetVersion(), setVersion);

    MG_Impl::GLImpl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, 8);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_FLOAT_EQ(samplerObject->GetMaxAnisotropy(), 8.0f);
    EXPECT_EQ(samplerObject->GetVersion(), static_cast<Uint16>(setVersion + 1));
}

TEST_F(TextureTest, TextureMaxAnisotropyBelowOneIsInvalidValueAndPreservesState) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    const auto textureObject = MG_State::pGLContext->GetTextureObject(texture);
    ASSERT_NE(textureObject, nullptr);
    const auto& samplerObject = textureObject->GetSamplerObject();
    ASSERT_NE(samplerObject, nullptr);
    const Uint16 initialVersion = samplerObject->GetVersion();

    MG_Impl::GLImpl::TexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, 0.5f);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_VALUE);
    EXPECT_FLOAT_EQ(samplerObject->GetMaxAnisotropy(), 1.0f);
    EXPECT_EQ(samplerObject->GetVersion(), initialVersion);

    MG_Impl::GLImpl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_VALUE);
    EXPECT_FLOAT_EQ(samplerObject->GetMaxAnisotropy(), 1.0f);
    EXPECT_EQ(samplerObject->GetVersion(), initialVersion);
}

TEST_F(TextureTest, SamplerMaxAnisotropyUsesTheSameStateAndValidationSemantics) {
    GLuint sampler = 0;
    MG_Impl::GLImpl::GenSamplers(1, &sampler);
    ASSERT_NE(sampler, 0u);

    GLfloat floatValue = 0.0f;
    MG_Impl::GLImpl::GetSamplerParameterfv(sampler, GL_TEXTURE_MAX_ANISOTROPY_EXT, &floatValue);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_FLOAT_EQ(floatValue, 1.0f);

    const auto& samplerObject = MG_State::pGLContext->GetSamplerObject(sampler);
    ASSERT_NE(samplerObject, nullptr);
    const Uint16 initialVersion = samplerObject->GetVersion();

    MG_Impl::GLImpl::SamplerParameterf(sampler, GL_TEXTURE_MAX_ANISOTROPY_EXT, 6.0f);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_FLOAT_EQ(samplerObject->GetMaxAnisotropy(), 6.0f);
    EXPECT_EQ(samplerObject->GetVersion(), static_cast<Uint16>(initialVersion + 1));

    GLint integerValue = 0;
    MG_Impl::GLImpl::GetSamplerParameteriv(sampler, GL_TEXTURE_MAX_ANISOTROPY_EXT, &integerValue);
    EXPECT_EQ(integerValue, 6);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    const Uint16 setVersion = samplerObject->GetVersion();
    MG_Impl::GLImpl::SamplerParameterf(sampler, GL_TEXTURE_MAX_ANISOTROPY_EXT, 6.0f);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_EQ(samplerObject->GetVersion(), setVersion);

    MG_Impl::GLImpl::SamplerParameterf(sampler, GL_TEXTURE_MAX_ANISOTROPY_EXT, 0.25f);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_VALUE);
    EXPECT_FLOAT_EQ(samplerObject->GetMaxAnisotropy(), 6.0f);
    EXPECT_EQ(samplerObject->GetVersion(), setVersion);

    MG_Impl::GLImpl::SamplerParameteri(sampler, GL_TEXTURE_MAX_ANISOTROPY_EXT, 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_VALUE);
    EXPECT_FLOAT_EQ(samplerObject->GetMaxAnisotropy(), 6.0f);
    EXPECT_EQ(samplerObject->GetVersion(), setVersion);

    const GLint signedInvalidValue = -1;
    MG_Impl::GLImpl::SamplerParameterIiv(sampler, GL_TEXTURE_MAX_ANISOTROPY_EXT, &signedInvalidValue);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_VALUE);
    EXPECT_FLOAT_EQ(samplerObject->GetMaxAnisotropy(), 6.0f);
    EXPECT_EQ(samplerObject->GetVersion(), setVersion);

    const GLuint unsignedValue = 10;
    MG_Impl::GLImpl::SamplerParameterIuiv(sampler, GL_TEXTURE_MAX_ANISOTROPY_EXT, &unsignedValue);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_FLOAT_EQ(samplerObject->GetMaxAnisotropy(), 10.0f);
    EXPECT_EQ(samplerObject->GetVersion(), static_cast<Uint16>(setVersion + 1));

    GLuint queriedUnsignedValue = 0;
    MG_Impl::GLImpl::GetSamplerParameterIuiv(sampler, GL_TEXTURE_MAX_ANISOTROPY_EXT, &queriedUnsignedValue);
    EXPECT_EQ(queriedUnsignedValue, unsignedValue);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// GL 3.3 core 3.8.2: BindSampler rejects a never-generated or already-deleted name with
// INVALID_OPERATION, while SamplerParameter* on the same name is INVALID_VALUE - the two paths
// must not share one validator. Delete of an unknown name stays silent.
TEST_F(TextureTest, BindSamplerRejectsUnknownNameWithInvalidOperationUnlikeSamplerParameter) {
    GLuint sampler = 0;
    MG_Impl::GLImpl::GenSamplers(1, &sampler);
    ASSERT_NE(sampler, 0u);
    MG_Impl::GLImpl::BindSampler(0, sampler);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // Deleting is silent, twice over, and the name is dead afterwards.
    MG_Impl::GLImpl::DeleteSamplers(1, &sampler);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    MG_Impl::GLImpl::DeleteSamplers(1, &sampler);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::BindSampler(0, sampler);
    ExpectSingleGlError(GL_INVALID_OPERATION);

    // Same dead name through SamplerParameter*: INVALID_VALUE, so the two paths cannot share one
    // validator - and neither may queue the other's code alongside its own.
    MG_Impl::GLImpl::SamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    ExpectSingleGlError(GL_INVALID_VALUE);
}

TEST_F(TextureTest, GenThenBindCreatesObjectForUnsizedPackedBgraSubImageUpload) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    ASSERT_NE(texture, 0u);
    ASSERT_TRUE(MG_State::pGLContext->ValidateTextureName(texture));
    // GenTextures only reserves the name; the object appears on first bind.
    ASSERT_FALSE(MG_State::pGLContext->ValidateTextureObject(texture));
    EXPECT_EQ(MG_Impl::GLImpl::IsTexture(texture), GL_FALSE);

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);

    const auto textureObject = MG_State::pGLContext->GetTextureObject(texture);
    ASSERT_NE(textureObject, nullptr);
    EXPECT_TRUE(MG_State::pGLContext->ValidateTextureObject(texture));
    EXPECT_EQ(MG_Impl::GLImpl::IsTexture(texture), GL_TRUE);

    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 1, 0, GL_BGRA,
                                GL_UNSIGNED_INT_8_8_8_8_REV, nullptr);
    const Uint8 pixels[] = {
        10, 20, 30, 40,
        50, 60, 70, 80,
    };
    MG_Impl::GLImpl::TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 2, 1, GL_BGRA,
                                   GL_UNSIGNED_INT_8_8_8_8_REV, pixels);

    const auto* stored = GetBoundTexture2DLevelBytes(texture);
    ASSERT_NE(stored, nullptr);
    const Uint8 expected[] = {
        30, 20, 10, 40,
        70, 60, 50, 80,
    };
    EXPECT_EQ(std::memcmp(stored, expected, sizeof(expected)), 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

namespace {
    // Strict core rules only apply when the current EGL context explicitly requested a core
    // profile; the suite's default (no current context) runs with relaxed semantics. RAII so
    // a failed ASSERT cannot leave the context current for the rest of the suite.
    struct ScopedCoreProfileContext {
        ScopedCoreProfileContext() {
            auto& egl = *MG_State::pEGLContext;
            m_display = egl.GetDisplay(EGL_DEFAULT_DISPLAY);
            EXPECT_NE(m_display, EGL_NO_DISPLAY);
            EXPECT_TRUE(egl.InitializeDisplay(m_display, nullptr, nullptr));
            EGLint configCount = 0;
            EXPECT_TRUE(egl.ChooseConfig(m_display, nullptr, &m_config, 1, &configCount));
            const EGLint surfaceAttribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
            m_surface = egl.CreatePbufferSurface(m_display, m_config, surfaceAttribs);
            EXPECT_NE(m_surface, EGL_NO_SURFACE);
            const EGLint contextAttribs[] = {EGL_CONTEXT_MAJOR_VERSION,
                                             3,
                                             EGL_CONTEXT_MINOR_VERSION,
                                             3,
                                             EGL_CONTEXT_OPENGL_PROFILE_MASK,
                                             EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                             EGL_NONE};
            m_context = egl.CreateContext(m_display, m_config, EGL_NO_CONTEXT, contextAttribs);
            EXPECT_NE(m_context, EGL_NO_CONTEXT);
            EXPECT_TRUE(egl.MakeCurrent(m_display, m_surface, m_surface, m_context));
        }
        ~ScopedCoreProfileContext() {
            auto& egl = *MG_State::pEGLContext;
            egl.MakeCurrent(EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (m_context != EGL_NO_CONTEXT) egl.DestroyContext(m_display, m_context);
            if (m_surface != EGL_NO_SURFACE) egl.DestroySurface(m_display, m_surface);
        }
        ScopedCoreProfileContext(const ScopedCoreProfileContext&) = delete;
        ScopedCoreProfileContext& operator=(const ScopedCoreProfileContext&) = delete;

    private:
        EGLDisplay m_display = EGL_NO_DISPLAY;
        EGLConfig m_config = nullptr;
        EGLSurface m_surface = EGL_NO_SURFACE;
        MG_State::EGLState::EGLContext::EGLContextHandle m_context = EGL_NO_CONTEXT;
    };

    // MOBILEGL_RELAXED_SEMANTICS loosens strict core rules even on explicit core-profile
    // contexts. RAII so a failed ASSERT cannot leak the flag into the rest of the suite.
    struct ScopedRelaxedSemantics {
        ScopedRelaxedSemantics(): m_previous(MG_Config::Features.RelaxedSemantics) {
            MG_Config::Features.RelaxedSemantics = true;
        }
        ~ScopedRelaxedSemantics() {
            MG_Config::Features.RelaxedSemantics = m_previous;
        }
        ScopedRelaxedSemantics(const ScopedRelaxedSemantics&) = delete;
        ScopedRelaxedSemantics& operator=(const ScopedRelaxedSemantics&) = delete;

    private:
        Bool m_previous;
    };
} // namespace

// GL 3.3 core 3.8.1: on an explicit core-profile context, DeleteTextures makes the name unused
// again whether or not a bind ever instantiated an object, so the reservation must go back to
// the generator's free list rather than leaking, and binding the dead name afterwards must fail.
TEST_F(TextureTest, DeleteGeneratedButUnboundNameReleasesReservationAndBindFails) {
    ScopedCoreProfileContext coreContext;
    ASSERT_FALSE(MG_State::IsRelaxedSemanticsActive());

    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    ASSERT_NE(texture, 0u);
    ASSERT_TRUE(MG_State::pGLContext->ValidateTextureName(texture));
    ASSERT_FALSE(MG_State::pGLContext->ValidateTextureObject(texture));

    MG_Impl::GLImpl::DeleteTextures(1, &texture);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_FALSE(MG_State::pGLContext->ValidateTextureName(texture));
    EXPECT_FALSE(MG_State::pGLContext->ValidateTextureObject(texture));
    // IsTexture answers about a dead name without raising anything (GL 3.3 core 6.1.4).
    EXPECT_EQ(MG_Impl::GLImpl::IsTexture(texture), GL_FALSE);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
    ExpectSingleGlError(GL_INVALID_OPERATION);
    EXPECT_FALSE(MG_State::pGLContext->ValidateTextureObject(texture));

    // The freed reservation is recycled (the generator's free list is LIFO, so the very same
    // name comes back) - a delete that skipped the release would hand out a fresh name here.
    GLuint recycled = 0;
    MG_Impl::GLImpl::GenTextures(1, &recycled);
    EXPECT_EQ(recycled, texture);
    EXPECT_TRUE(MG_State::pGLContext->ValidateTextureName(recycled));
}

// Relaxed semantics - the default whenever the context did not explicitly request a core
// profile: legacy Minecraft reserves a texture name, deletes it before first bind, then reuses
// the same name for the atlas upload. Preserve that generated reservation so the later bind can
// instantiate the object and subsequent sub-image uploads target it instead of the default
// texture. Explicit core contexts keep the strict delete semantics asserted above.
TEST_F(TextureTest, RelaxedDefaultDeleteGeneratedReservationThenBindCreatesObjectForSubImageUpload) {
    ASSERT_TRUE(MG_State::IsRelaxedSemanticsActive());

    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    ASSERT_NE(texture, 0u);
    ASSERT_TRUE(MG_State::pGLContext->ValidateTextureName(texture));
    ASSERT_FALSE(MG_State::pGLContext->ValidateTextureObject(texture));

    MG_Impl::GLImpl::DeleteTextures(1, &texture);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_TRUE(MG_State::pGLContext->ValidateTextureName(texture));
    EXPECT_FALSE(MG_State::pGLContext->ValidateTextureObject(texture));
    EXPECT_EQ(MG_Impl::GLImpl::IsTexture(texture), GL_FALSE);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);

    const auto textureObject = MG_State::pGLContext->GetTextureObject(texture);
    ASSERT_NE(textureObject, nullptr);
    EXPECT_TRUE(MG_State::pGLContext->ValidateTextureName(texture));
    EXPECT_TRUE(MG_State::pGLContext->ValidateTextureObject(texture));
    EXPECT_EQ(MG_Impl::GLImpl::IsTexture(texture), GL_TRUE);

    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 1, 0, GL_BGRA,
                                GL_UNSIGNED_INT_8_8_8_8_REV, nullptr);
    const Uint8 pixels[] = {
        10, 20, 30, 40,
        50, 60, 70, 80,
    };
    MG_Impl::GLImpl::TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 2, 1, GL_BGRA,
                                   GL_UNSIGNED_INT_8_8_8_8_REV, pixels);

    const auto* stored = GetBoundTexture2DLevelBytes(texture);
    ASSERT_NE(stored, nullptr);
    const Uint8 expected[] = {
        30, 20, 10, 40,
        70, 60, 50, 80,
    };
    EXPECT_EQ(std::memcmp(stored, expected, sizeof(expected)), 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// MOBILEGL_RELAXED_SEMANTICS wins even on an explicit core-profile context: the deleted
// reservation survives and the name stays bindable.
TEST_F(TextureTest, RelaxedSemanticsOverrideKeepsDeletedReservationOnCoreProfileContext) {
    ScopedCoreProfileContext coreContext;
    ScopedRelaxedSemantics relaxedSemantics;
    ASSERT_TRUE(MG_State::IsRelaxedSemanticsActive());

    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    ASSERT_NE(texture, 0u);
    MG_Impl::GLImpl::DeleteTextures(1, &texture);
    EXPECT_TRUE(MG_State::pGLContext->ValidateTextureName(texture));

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
    EXPECT_TRUE(MG_State::pGLContext->ValidateTextureObject(texture));
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, 0);
    MG_Impl::GLImpl::DeleteTextures(1, &texture);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, DeleteInstantiatedTextureInvalidatesNameUntilRegenerated) {
    GLuint textures[2] = {};
    MG_Impl::GLImpl::GenTextures(2, textures);
    ASSERT_NE(textures[0], 0u);
    ASSERT_NE(textures[1], 0u);

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, textures[0]);
    ASSERT_TRUE(MG_State::pGLContext->ValidateTextureObject(textures[0]));
    MG_Impl::GLImpl::DeleteTextures(1, &textures[0]);

    EXPECT_FALSE(MG_State::pGLContext->ValidateTextureName(textures[0]));
    EXPECT_FALSE(MG_State::pGLContext->ValidateTextureObject(textures[0]));

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, textures[1]);
    const auto fallbackObject = MG_State::pGLContext->GetTextureObject(textures[1]);
    ASSERT_NE(fallbackObject, nullptr);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, textures[0]);
    ExpectSingleGlError(GL_INVALID_OPERATION);
    EXPECT_EQ(MG_State::pGLContext->GetTextureUnitObject(0)
                  .GetBindingSlot(TextureTarget::Texture2D)
                  .GetBoundObject(),
              fallbackObject);
}

TEST_F(TextureTest, DeleteUnknownNamesIsSilentButBindUnknownNameIsInvalid) {
    GLuint validTexture = 0;
    MG_Impl::GLImpl::GenTextures(1, &validTexture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, validTexture);
    const auto boundObject = MG_State::pGLContext->GetTextureObject(validTexture);
    ASSERT_NE(boundObject, nullptr);

    constexpr GLuint unknownNames[] = {0, std::numeric_limits<GLuint>::max()};
    MG_Impl::GLImpl::DeleteTextures(2, unknownNames);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, unknownNames[1]);
    ExpectSingleGlError(GL_INVALID_OPERATION);
    EXPECT_EQ(MG_State::pGLContext->GetTextureUnitObject(0)
                  .GetBindingSlot(TextureTarget::Texture2D)
                  .GetBoundObject(),
              boundObject);
    EXPECT_FALSE(MG_State::pGLContext->ValidateTextureName(unknownNames[1]));
    EXPECT_FALSE(MG_State::pGLContext->ValidateTextureObject(unknownNames[1]));
}

TEST_F(TextureTest, BindTextureUnitEnumAsNameIsSilentNoOp) {
    GLuint validTexture = 0;
    MG_Impl::GLImpl::GenTextures(1, &validTexture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, validTexture);
    const auto boundObject = MG_State::pGLContext->GetTextureObject(validTexture);
    ASSERT_NE(boundObject, nullptr);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    constexpr GLuint textureUnitEnum = GL_TEXTURE7;
    ASSERT_FALSE(MG_State::pGLContext->ValidateTextureName(textureUnitEnum));
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, textureUnitEnum);

    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_EQ(MG_State::pGLContext->GetTextureUnitObject(0)
                  .GetBindingSlot(TextureTarget::Texture2D)
                  .GetBoundObject(),
              boundObject);
    EXPECT_FALSE(MG_State::pGLContext->ValidateTextureName(textureUnitEnum));
    EXPECT_FALSE(MG_State::pGLContext->ValidateTextureObject(textureUnitEnum));
}

TEST_F(TextureTest, TexSubImage2DOnImagelessDefaultTextureReportsErrorInsteadOfDereferencingNull) {
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, 0);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // Texture zero is a real (default) texture object now, so TexSubImage2D no longer fails for
    // want of a bound object - it fails because the region exceeds the default texture's (empty
    // or zero-sized) level 0, which is GL_INVALID_VALUE per GL 3.3 core 3.8.2.
    const Uint8 pixel[] = {1, 2, 3, 4};
    MG_Impl::GLImpl::TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);

    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_VALUE);
}

TEST_F(TextureTest, TextureStorageAndSubImageModifyNamedObjectOnly) {
    GLuint namedTexture = 0;
    GLuint boundTexture = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &namedTexture);
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &boundTexture);
    MG_Impl::GLImpl::BindTextureUnit(0, boundTexture);

    const auto boundObjectBefore =
        MG_State::pGLContext->GetTextureUnitObject(0).GetBindingSlot(TextureTarget::Texture2D).GetBoundObject();

    MG_Impl::GLImpl::TextureStorage2D(namedTexture, 2, GL_RGBA8, 2, 2);
    const Uint8 pixels[] = {
        1, 2, 3, 4,
        5, 6, 7, 8,
        9, 10, 11, 12,
        13, 14, 15, 16,
    };
    MG_Impl::GLImpl::TextureSubImage2D(namedTexture, 0, 0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    const auto namedObject = MG_State::pGLContext->GetTextureObject(namedTexture);
    auto* mipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(namedObject.get());
    EXPECT_EQ(mipmapObject->GetMipmapTexelSize(TextureUploadTarget::Texture2D, 0), IntVec3(2, 2, 1));
    EXPECT_EQ(mipmapObject->GetMipmapTexelSize(TextureUploadTarget::Texture2D, 1), IntVec3(1, 1, 1));
    EXPECT_TRUE(mipmapObject->IsStorageDirty(TextureUploadTarget::Texture2D, 0));
    EXPECT_FALSE(mipmapObject->IsStorageDirty(TextureUploadTarget::Texture2D, 1));

    EXPECT_EQ(MG_State::pGLContext->GetTextureUnitObject(0).GetBindingSlot(TextureTarget::Texture2D).GetBoundObject(),
              boundObjectBefore);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, BoundTexSubImage2DUsesCompactRowsAfterUnpackProcessing) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);

    const Uint8 initialPixels[2 * 16] = {};
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 5, 2, 0, GL_RGB, GL_UNSIGNED_BYTE, initialPixels);

    const Uint8 subImageWithGuard[] = {
        1, 2, 3, 4, 5, 6, 7, 8, 9,
        101, 102, 103,
        10, 11, 12, 13, 14, 15, 16, 17, 18,
        201, 202, 203, 204, 205, 206,
    };
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    MG_Impl::GLImpl::TexSubImage2D(GL_TEXTURE_2D, 0, 1, 0, 3, 2, GL_RGB, GL_UNSIGNED_BYTE, subImageWithGuard);

    const auto textureObject = MG_State::pGLContext->GetTextureObject(texture);
    auto* mipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
    const auto* stored = static_cast<const Uint8*>(
        mipmapObject->MapMipmapData(TextureUploadTarget::Texture2D, 0));

    const Uint8 expected[] = {
        0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 0, 0,
        0, 0, 0, 10, 11, 12, 13, 14, 15, 16, 17, 18, 0, 0, 0,
    };
    for (SizeT i = 0; i < sizeof(expected); ++i) {
        EXPECT_EQ(stored[i], expected[i]) << "byte " << i;
    }
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, BoundTexSubImage2DUnpacksPackedBgra8888ToRgba8) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 1, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8, nullptr);

    const Uint8 pixels[] = {
        10, 20, 30, 40,
        50, 60, 70, 80,
    };
    MG_Impl::GLImpl::TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 2, 1, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8, pixels);

    const auto* stored = GetBoundTexture2DLevelBytes(texture);
    const Uint8 expected[] = {
        20, 30, 40, 10,
        60, 70, 80, 50,
    };
    for (SizeT i = 0; i < sizeof(expected); ++i) {
        EXPECT_EQ(stored[i], expected[i]) << "byte " << i;
    }
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, BoundTexImage2DUnpacksPackedBgra8888ToRgba8WithPixelStoreSkips) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);

    const Uint8 pixels[] = {
        1, 2, 3, 4,
        5, 6, 7, 8,
        9, 10, 11, 12,
        13, 14, 15, 16,
        17, 18, 19, 20,
        10, 20, 30, 40,
        50, 60, 70, 80,
        21, 22, 23, 24,
        25, 26, 27, 28,
        90, 100, 110, 120,
        130, 140, 150, 160,
        29, 30, 31, 32,
    };

    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_ROW_LENGTH, 4);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_SKIP_PIXELS, 1);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_SKIP_ROWS, 1);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8, pixels);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_SKIP_ROWS, 0);

    const auto* stored = GetBoundTexture2DLevelBytes(texture);
    const Uint8 expected[] = {
        20, 30, 40, 10,
        60, 70, 80, 50,
        100, 110, 120, 90,
        140, 150, 160, 130,
    };
    for (SizeT i = 0; i < sizeof(expected); ++i) {
        EXPECT_EQ(stored[i], expected[i]) << "byte " << i;
    }
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// GL CTS packed_pixels feeds every format/type/internalformat combination to TexImage and expects
// GL_INVALID_OPERATION for the invalid ones; these used to slip through validation and SIGTRAP in
// the shadow-storage upload path.
TEST_F(TextureTest, TexImage2DRejectsMismatchedFormatCombinations) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);

    // Depth-stencil internal format with a color format.
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, 2, 2, 0, GL_BGR, GL_UNSIGNED_BYTE, nullptr);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_OPERATION);

    // Color internal format with a depth format.
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_OPERATION);

    // Stencil-only uploads do not exist in core 3.3.
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, 2, 2, 0, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE,
                                nullptr);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_OPERATION);

    // Packed depth-stencil type with a color format.
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_INT_24_8, nullptr);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_OPERATION);

    // DEPTH_STENCIL format requires one of the two packed depth-stencil types.
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, 2, 2, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_BYTE,
                                nullptr);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_OPERATION);

    // Integer-ness of format and internal format must match (both directions).
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, nullptr);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_OPERATION);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8UI, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_OPERATION);

    // Integer formats cannot be paired with floating-point types.
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32I, 2, 2, 0, GL_RGBA_INTEGER, GL_FLOAT, nullptr);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_OPERATION);

    // UNSIGNED_INT_5_9_9_9_REV pairs with RGB only.
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGB9_E5, 2, 2, 0, GL_RGBA, GL_UNSIGNED_INT_5_9_9_9_REV, nullptr);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_OPERATION);
}

TEST_F(TextureTest, TexImage2DAcceptsSpecCompliantFormatCombinations) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);

    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, 2, 2, 0, GL_DEPTH_STENCIL,
                                GL_UNSIGNED_INT_24_8, nullptr);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // Depth-component internal format accepts DEPTH_STENCIL input (stencil bits are dropped).
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT16, 2, 2, 0, GL_DEPTH_STENCIL,
                                GL_UNSIGNED_INT_24_8, nullptr);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8UI, 2, 2, 0, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, nullptr);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // Packed RGB types allow the integer variant of the RGB format.
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGB8UI, 2, 2, 0, GL_RGB_INTEGER, GL_UNSIGNED_BYTE_3_3_2,
                                nullptr);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGB9_E5, 2, 2, 0, GL_RGB, GL_UNSIGNED_INT_5_9_9_9_REV, nullptr);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// Desktop GL table 3.3 lists GREEN and BLUE as TexImage client formats (GL CTS packed_pixels
// rgba8_format_green/blue upload with them and verify the readback): the single input component
// feeds the named channel, the other color channels default to 0 and alpha to 1.
TEST_F(TextureTest, BoundTexImage2DUnpacksGreenAndBlueIntoRgba8Channels) {
    GLuint greenTexture = 0;
    MG_Impl::GLImpl::GenTextures(1, &greenTexture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, greenTexture);

    const Uint8 pixels[] = {
        10, 20,
        30, 40,
    };
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_GREEN, GL_UNSIGNED_BYTE, pixels);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    const auto* storedGreen = GetBoundTexture2DLevelBytes(greenTexture);
    const Uint8 expectedGreen[] = {
        0, 10, 0, 255,
        0, 20, 0, 255,
        0, 30, 0, 255,
        0, 40, 0, 255,
    };
    for (SizeT i = 0; i < sizeof(expectedGreen); ++i) {
        EXPECT_EQ(storedGreen[i], expectedGreen[i]) << "byte " << i;
    }

    GLuint blueTexture = 0;
    MG_Impl::GLImpl::GenTextures(1, &blueTexture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, blueTexture);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_BLUE, GL_UNSIGNED_BYTE, pixels);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    const auto* storedBlue = GetBoundTexture2DLevelBytes(blueTexture);
    const Uint8 expectedBlue[] = {
        0, 0, 10, 255,
        0, 0, 20, 255,
        0, 0, 30, 255,
        0, 0, 40, 255,
    };
    for (SizeT i = 0; i < sizeof(expectedBlue); ++i) {
        EXPECT_EQ(storedBlue[i], expectedBlue[i]) << "byte " << i;
    }
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, BoundTexImage2DUnpacksGreenIntegerIntoRgba8UiChannels) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);

    const Uint8 pixels[] = {
        10, 20,
        30, 40,
    };
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8UI, 2, 2, 0, GL_GREEN_INTEGER, GL_UNSIGNED_BYTE, pixels);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    const auto* stored = GetBoundTexture2DLevelBytes(texture);
    // Missing integer channels default to R=0, B=0, A=1.
    const Uint8 expected[] = {
        0, 10, 0, 1,
        0, 20, 0, 1,
        0, 30, 0, 1,
        0, 40, 0, 1,
    };
    for (SizeT i = 0; i < sizeof(expected); ++i) {
        EXPECT_EQ(stored[i], expected[i]) << "byte " << i;
    }
}

TEST_F(TextureTest, TexImage2DSingleChannelFormatsKeepIntegerNessRules) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);

    // Integer-ness of format and internal format must match (both directions).
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_GREEN_INTEGER, GL_UNSIGNED_BYTE, nullptr);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_OPERATION);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8UI, 2, 2, 0, GL_BLUE, GL_UNSIGNED_BYTE, nullptr);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_OPERATION);

    // Integer formats reject floating-point types.
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8UI, 2, 2, 0, GL_BLUE_INTEGER, GL_FLOAT, nullptr);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_OPERATION);

    // Packed types never pair with single-channel formats.
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_GREEN, GL_UNSIGNED_SHORT_5_6_5, nullptr);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_OPERATION);
}

TEST_F(TextureTest, TexImage3DRejectsDepthFormatsForThreeDimensionalTarget) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_3D, texture);
    MG_Impl::GLImpl::TexImage3D(GL_TEXTURE_3D, 0, GL_DEPTH24_STENCIL8, 2, 2, 2, 0, GL_DEPTH_STENCIL,
                                GL_UNSIGNED_INT_24_8, nullptr);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_OPERATION);

    // 2D-array targets remain valid for depth formats.
    GLuint arrayTexture = 0;
    MG_Impl::GLImpl::GenTextures(1, &arrayTexture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D_ARRAY, arrayTexture);
    MG_Impl::GLImpl::TexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH24_STENCIL8, 2, 2, 2, 0, GL_DEPTH_STENCIL,
                                GL_UNSIGNED_INT_24_8, nullptr);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, BoundTexSubImage2DUnpacksPackedBgra8888RevToRgba8) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 1, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, nullptr);

    const Uint8 pixels[] = {
        10, 20, 30, 40,
        50, 60, 70, 80,
    };
    MG_Impl::GLImpl::TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 2, 1, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, pixels);

    const auto* stored = GetBoundTexture2DLevelBytes(texture);
    const Uint8 expected[] = {
        30, 20, 10, 40,
        70, 60, 50, 80,
    };
    for (SizeT i = 0; i < sizeof(expected); ++i) {
        EXPECT_EQ(stored[i], expected[i]) << "byte " << i;
    }
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, UnsizedRgbaInfersRgba8ForPacked8888Types) {
    EXPECT_EQ(MG_Util::ConvertInternalFormatToSized(TextureInternalFormat::RGBA, TextureInputFormat::BGRA,
                                                    TexturePixelDataType::UnsignedInt8888),
              TextureInternalFormat::RGBA8);
    EXPECT_EQ(MG_Util::ConvertInternalFormatToSized(TextureInternalFormat::RGBA, TextureInputFormat::BGRA,
                                                    TexturePixelDataType::UnsignedInt8888Rev),
              TextureInternalFormat::RGBA8);
}

TEST_F(TextureTest, BoundTexImageAndSubImage2DUseInferredRgba8ForPackedBgra8888Rev) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);

    const Uint8 initialPixels[] = {
        10, 20, 30, 40,
        50, 60, 70, 80,
    };
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 1, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
                                initialPixels);

    const auto textureObject = MG_State::pGLContext->GetTextureObject(texture);
    EXPECT_EQ(textureObject->GetFormat(), TextureInternalFormat::RGBA8);
    const auto* stored = GetBoundTexture2DLevelBytes(texture);
    const Uint8 expectedInitial[] = {
        30, 20, 10, 40,
        70, 60, 50, 80,
    };
    for (SizeT i = 0; i < sizeof(expectedInitial); ++i) {
        EXPECT_EQ(stored[i], expectedInitial[i]) << "initial byte " << i;
    }

    const Uint8 updatedPixels[] = {
        90, 100, 110, 120,
        130, 140, 150, 160,
    };
    MG_Impl::GLImpl::TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 2, 1, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
                                   updatedPixels);

    stored = GetBoundTexture2DLevelBytes(texture);
    const Uint8 expectedUpdated[] = {
        110, 100, 90, 120,
        150, 140, 130, 160,
    };
    for (SizeT i = 0; i < sizeof(expectedUpdated); ++i) {
        EXPECT_EQ(stored[i], expectedUpdated[i]) << "updated byte " << i;
    }
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, BoundTexSubImage2DUnpacksPackedRgba8888ToRgba8) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 1, 0, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8, nullptr);

    const Uint8 pixels[] = {
        10, 20, 30, 40,
        50, 60, 70, 80,
    };
    MG_Impl::GLImpl::TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 2, 1, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8, pixels);

    const auto* stored = GetBoundTexture2DLevelBytes(texture);
    const Uint8 expected[] = {
        40, 30, 20, 10,
        80, 70, 60, 50,
    };
    for (SizeT i = 0; i < sizeof(expected); ++i) {
        EXPECT_EQ(stored[i], expected[i]) << "byte " << i;
    }
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, BoundTexSubImage2DKeepsPackedRgba8888RevAsRgba8) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 1, 0, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8_REV, nullptr);

    const Uint8 pixels[] = {
        10, 20, 30, 40,
        50, 60, 70, 80,
    };
    MG_Impl::GLImpl::TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 2, 1, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8_REV, pixels);

    const auto* stored = GetBoundTexture2DLevelBytes(texture);
    EXPECT_EQ(std::memcmp(stored, pixels, sizeof(pixels)), 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, BoundTexStorage2DAllocatesRedTextureForSubImageUpdates) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);

    MG_Impl::GLImpl::TexStorage2D(GL_TEXTURE_2D, 1, GL_R8, 32, 32);

    const auto textureObject = MG_State::pGLContext->GetTextureObject(texture);
    auto* mipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
    ASSERT_NE(mipmapObject, nullptr);
    EXPECT_EQ(mipmapObject->GetMipmapTexelSize(TextureUploadTarget::Texture2D, 0), IntVec3(32, 32, 1));
    EXPECT_TRUE(textureObject->IsComplete());

    const Uint8 pixels[4 * 4] = {
        1, 2, 3, 4,
        5, 6, 7, 8,
        9, 10, 11, 12,
        13, 14, 15, 16,
    };
    MG_Impl::GLImpl::TexSubImage2D(GL_TEXTURE_2D, 0, 20, 28, 4, 4, GL_RED, GL_UNSIGNED_BYTE, pixels);

    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, TextureStorage2DMultisampleTracksNamedObjectState) {
    GLuint texture = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D_MULTISAMPLE, 1, &texture);

    constexpr GLsizei sampleCount = 1;
    MG_Impl::GLImpl::TextureStorage2DMultisample(texture, sampleCount, GL_RGBA8, 8, 6, GL_TRUE);

    const auto textureObject = MG_State::pGLContext->GetTextureObject(texture);
    ASSERT_NE(textureObject, nullptr);
    EXPECT_EQ(textureObject->GetTarget(), TextureTarget::Texture2DMultisample);
    EXPECT_EQ(textureObject->GetSamples(), sampleCount);
    EXPECT_TRUE(textureObject->HasFixedSampleLocations());

    auto* textureMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
    ASSERT_NE(textureMipmapObject, nullptr);
    EXPECT_EQ(textureMipmapObject->GetMipmapLevelCount(), 1u);
    EXPECT_EQ(textureMipmapObject->GetMipmapTexelSize(TextureUploadTarget::Texture2DMultisample, 0), IntVec3(8, 6, 1));
    EXPECT_FALSE(textureMipmapObject->IsStorageDirty(TextureUploadTarget::Texture2DMultisample, 0));

    GLint samples = 0;
    GLint fixed = 0;
    MG_Impl::GLImpl::GetTextureLevelParameteriv(texture, 0, GL_TEXTURE_SAMPLES, &samples);
    MG_Impl::GLImpl::GetTextureLevelParameteriv(texture, 0, GL_TEXTURE_FIXED_SAMPLE_LOCATIONS, &fixed);
    EXPECT_EQ(samples, sampleCount);
    EXPECT_EQ(fixed, GL_TRUE);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, GetTextureImageReadsNamedObjectWithoutBinding) {
    GLuint texture = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &texture);
    MG_Impl::GLImpl::TextureStorage2D(texture, 1, GL_RGBA8, 2, 1);

    const Uint8 pixels[] = {
        21, 22, 23, 24,
        31, 32, 33, 34,
    };
    MG_Impl::GLImpl::TextureSubImage2D(texture, 0, 0, 0, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    Uint8 output[sizeof(pixels)] = {};
    MG_Impl::GLImpl::GetTextureImage(texture, 0, GL_RGBA, GL_UNSIGNED_BYTE, sizeof(output), output);

    EXPECT_EQ(std::memcmp(output, pixels, sizeof(pixels)), 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, GetTextureSubImageReadsFullNamedLevelWithoutBinding) {
    GLuint texture = 0;
    GLuint boundTexture = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &texture);
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &boundTexture);
    MG_Impl::GLImpl::BindTextureUnit(0, boundTexture);

    const auto boundObjectBefore =
        MG_State::pGLContext->GetTextureUnitObject(0).GetBindingSlot(TextureTarget::Texture2D).GetBoundObject();

    MG_Impl::GLImpl::TextureStorage2D(texture, 1, GL_RGBA8, 2, 1);
    const Uint8 pixels[] = {
        41, 42, 43, 44,
        51, 52, 53, 54,
    };
    MG_Impl::GLImpl::TextureSubImage2D(texture, 0, 0, 0, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    Uint8 output[sizeof(pixels)] = {};
    MG_Impl::GLImpl::GetTextureSubImage(texture, 0, 0, 0, 0, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                        sizeof(output), output);

    EXPECT_EQ(std::memcmp(output, pixels, sizeof(pixels)), 0);
    EXPECT_EQ(MG_State::pGLContext->GetTextureUnitObject(0).GetBindingSlot(TextureTarget::Texture2D).GetBoundObject(),
              boundObjectBefore);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, GetTextureSubImageRejectsPartialReadbackForNow) {
    GLuint texture = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &texture);
    MG_Impl::GLImpl::TextureStorage2D(texture, 1, GL_RGBA8, 2, 2);

    Uint8 output[4] = {};
    MG_Impl::GLImpl::GetTextureSubImage(texture, 0, 0, 0, 0, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                        sizeof(output), output);

    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_OPERATION);
}

TEST_F(TextureTest, TextureParameteriAndBindTextureUnitAreDirectStateAccess) {
    GLuint texture = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &texture);

    MG_Impl::GLImpl::TextureParameteri(texture, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    GLint minFilter = 0;
    MG_Impl::GLImpl::GetTextureParameteriv(texture, GL_TEXTURE_MIN_FILTER, &minFilter);
    EXPECT_EQ(minFilter, GL_NEAREST);

    MG_Impl::GLImpl::BindTextureUnit(3, texture);
    EXPECT_EQ(MG_State::pGLContext->GetActiveTextureUnit(), 0);
    EXPECT_EQ(MG_State::pGLContext->GetTextureUnitObject(3)
                  .GetBindingSlot(TextureTarget::Texture2D)
                  .GetBoundObject()
                  ->GetExternalIndex(),
              texture);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, TextureParameterfModifiesNamedObjectWithoutBinding) {
    GLuint namedTexture = 0;
    GLuint boundTexture = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &namedTexture);
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &boundTexture);
    MG_Impl::GLImpl::BindTextureUnit(0, boundTexture);

    const auto boundObjectBefore =
        MG_State::pGLContext->GetTextureUnitObject(0).GetBindingSlot(TextureTarget::Texture2D).GetBoundObject();

    MG_Impl::GLImpl::TextureParameterf(namedTexture, GL_TEXTURE_MIN_FILTER, static_cast<GLfloat>(GL_LINEAR));
    MG_Impl::GLImpl::TextureParameterf(namedTexture, GL_TEXTURE_MAG_FILTER, static_cast<GLfloat>(GL_NEAREST));
    MG_Impl::GLImpl::TextureParameterf(namedTexture, GL_DEPTH_STENCIL_TEXTURE_MODE,
                                       static_cast<GLfloat>(GL_DEPTH_COMPONENT));

    GLint namedMinFilter = 0;
    GLint namedMagFilter = 0;
    GLint boundMinFilter = 0;
    GLint boundMagFilter = 0;
    MG_Impl::GLImpl::GetTextureParameteriv(namedTexture, GL_TEXTURE_MIN_FILTER, &namedMinFilter);
    MG_Impl::GLImpl::GetTextureParameteriv(namedTexture, GL_TEXTURE_MAG_FILTER, &namedMagFilter);
    MG_Impl::GLImpl::GetTextureParameteriv(boundTexture, GL_TEXTURE_MIN_FILTER, &boundMinFilter);
    MG_Impl::GLImpl::GetTextureParameteriv(boundTexture, GL_TEXTURE_MAG_FILTER, &boundMagFilter);

    EXPECT_EQ(namedMinFilter, GL_LINEAR);
    EXPECT_EQ(namedMagFilter, GL_NEAREST);
    EXPECT_EQ(boundMinFilter, GL_NEAREST_MIPMAP_LINEAR);
    EXPECT_EQ(boundMagFilter, GL_LINEAR);
    EXPECT_EQ(MG_State::pGLContext->GetTextureUnitObject(0).GetBindingSlot(TextureTarget::Texture2D).GetBoundObject(),
              boundObjectBefore);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, TextureStorage1DAndSubImageModifyNamedObjectOnly) {
    GLuint namedTexture = 0;
    GLuint boundTexture = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_1D, 1, &namedTexture);
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_1D, 1, &boundTexture);
    MG_Impl::GLImpl::BindTextureUnit(0, boundTexture);

    const auto boundObjectBefore =
        MG_State::pGLContext->GetTextureUnitObject(0).GetBindingSlot(TextureTarget::Texture1D).GetBoundObject();

    MG_Impl::GLImpl::TextureStorage1D(namedTexture, 2, GL_RGBA8, 4);
    const Uint8 pixels[] = {
        1, 2, 3, 4,
        5, 6, 7, 8,
        9, 10, 11, 12,
        13, 14, 15, 16,
    };
    MG_Impl::GLImpl::TextureSubImage1D(namedTexture, 0, 0, 4, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    const auto textureObject = MG_State::pGLContext->GetTextureObject(namedTexture);
    auto* mipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
    ASSERT_NE(mipmapObject, nullptr);
    EXPECT_EQ(mipmapObject->GetMipmapTexelSize(TextureUploadTarget::Texture1D, 0), IntVec3(4, 1, 1));
    EXPECT_EQ(mipmapObject->GetMipmapTexelSize(TextureUploadTarget::Texture1D, 1), IntVec3(2, 1, 1));
    EXPECT_TRUE(mipmapObject->IsStorageDirty(TextureUploadTarget::Texture1D, 0));

    const auto* stored = static_cast<const Uint8*>(mipmapObject->MapMipmapData(TextureUploadTarget::Texture1D, 0));
    ASSERT_NE(stored, nullptr);
    EXPECT_EQ(std::memcmp(stored, pixels, sizeof(pixels)), 0);

    EXPECT_EQ(MG_State::pGLContext->GetTextureUnitObject(0).GetBindingSlot(TextureTarget::Texture1D).GetBoundObject(),
              boundObjectBefore);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// Building a mip chain top-down - upload level N, then level 0 - must not destroy the levels
// already uploaded. AllocateLevel used to resize() the storage down to level+1 on every call, so
// the level-0 upload truncated the chain to a single level; the higher level then read back as
// {0,0,0}, IsComplete() rejected the zero-then-nonzero pattern, and DirectGLES answered that by
// skipping the texture's sync entirely. This is the shape KHR-GL33.texture_repeat_mode uses, and
// it accounted for 108 CTS failures in every GL version.
TEST_F(TextureTest, TexImage2DOnLevelZeroKeepsAnAlreadyUploadedHigherLevel) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);

    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 49, 23, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 98, 46, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    const auto textureObject = MG_State::pGLContext->GetTextureObject(texture);
    auto* mipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
    ASSERT_NE(mipmapObject, nullptr);
    EXPECT_EQ(mipmapObject->GetMipmapLevelCount(), 2u);
    EXPECT_EQ(mipmapObject->GetMipmapTexelSize(TextureUploadTarget::Texture2D, 0), IntVec3(98, 46, 1));
    EXPECT_EQ(mipmapObject->GetMipmapTexelSize(TextureUploadTarget::Texture2D, 1), IntVec3(49, 23, 1));
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// The other half of the contract: respecifying a level 0 that already held an image still drops
// the chain, exactly as before. Minecraft rebinds the block-atlas name and calls glTexImage2D on
// level 0 before uploading the new levels; leaving the previous chain in place would strand a tail
// at the wrong sizes and - because Mojang terminates its chains with a 0x0 level - reproduce the
// same incomplete-texture black atlas the fix above exists to prevent.
TEST_F(TextureTest, TexImage2DRespecifyingAnExistingLevelZeroDropsTheStaleChain) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);

    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 2, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    const auto textureObject = MG_State::pGLContext->GetTextureObject(texture);
    auto* mipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
    ASSERT_NE(mipmapObject, nullptr);
    ASSERT_EQ(mipmapObject->GetMipmapLevelCount(), 3u);

    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    EXPECT_EQ(mipmapObject->GetMipmapLevelCount(), 1u);
    EXPECT_EQ(mipmapObject->GetMipmapTexelSize(TextureUploadTarget::Texture2D, 0), IntVec3(16, 16, 1));
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// Same-size respecification has to drop the chain too. The Mipmap Levels video setting rebuilds
// the atlas at identical dimensions with a different level count, so a size-change-only test would
// let the old tail survive.
TEST_F(TextureTest, TexImage2DRespecifyingLevelZeroAtTheSameSizeStillDropsTheChain) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);

    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    const auto textureObject = MG_State::pGLContext->GetTextureObject(texture);
    auto* mipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
    ASSERT_NE(mipmapObject, nullptr);
    EXPECT_EQ(mipmapObject->GetMipmapLevelCount(), 1u);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// glTexStorage2D defines exactly `levels` levels. AllocateStorage only grows now, so the immutable
// path has to drop a longer pre-existing chain explicitly.
TEST_F(TextureTest, TexStorage2DTrimsALongerPreExistingMipChain) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);

    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 2, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 3, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    MG_Impl::GLImpl::TexStorage2D(GL_TEXTURE_2D, 2, GL_RGBA8, 8, 8);

    const auto textureObject = MG_State::pGLContext->GetTextureObject(texture);
    auto* mipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
    ASSERT_NE(mipmapObject, nullptr);
    EXPECT_EQ(mipmapObject->GetMipmapLevelCount(), 2u);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// glTexImage2D used to reject every GL_COMPRESSED_* internal format with GL_INVALID_ENUM, because
// none of them mapped to a TextureInternalFormat and the "unknown format" gate fired. They now
// resolve to the uncompressed storage that backs them - what GL prescribes for the generic formats,
// and a deliberate deviation for RGTC, which ES cannot compress. The (format, type) pairs below are
// the ones KHR-GL33.packed_pixels uploads with, so this table doubles as a pin for those 480 cases.
TEST_F(TextureTest, CompressedInternalFormatsResolveToTheirUncompressedStorage) {
    struct Case {
        GLenum internalFormat;
        GLenum format;
        GLenum type;
        TextureInternalFormat expected;
    };
    const Case cases[] = {
        {GL_COMPRESSED_RED, GL_RED, GL_UNSIGNED_BYTE, TextureInternalFormat::R8},
        {GL_COMPRESSED_RG, GL_RG, GL_UNSIGNED_BYTE, TextureInternalFormat::RG8},
        {GL_COMPRESSED_RGB, GL_RGB, GL_UNSIGNED_BYTE, TextureInternalFormat::RGB8},
        {GL_COMPRESSED_RGBA, GL_RGBA, GL_UNSIGNED_BYTE, TextureInternalFormat::RGBA8},
        {GL_COMPRESSED_SRGB, GL_RGB, GL_UNSIGNED_BYTE, TextureInternalFormat::SRGB8},
        {GL_COMPRESSED_SRGB_ALPHA, GL_RGBA, GL_UNSIGNED_BYTE, TextureInternalFormat::SRGB8Alpha8},
        {GL_COMPRESSED_RED_RGTC1, GL_RED, GL_UNSIGNED_BYTE, TextureInternalFormat::R8},
        {GL_COMPRESSED_RG_RGTC2, GL_RG, GL_UNSIGNED_BYTE, TextureInternalFormat::RG8},
        // The signed RGTC pair is uploaded as GL_BYTE and must land on SNORM storage - resolving
        // them to plain R8/RG8 would silently reinterpret negative texels.
        {GL_COMPRESSED_SIGNED_RED_RGTC1, GL_RED, GL_BYTE, TextureInternalFormat::R8Snorm},
        {GL_COMPRESSED_SIGNED_RG_RGTC2, GL_RG, GL_BYTE, TextureInternalFormat::RG8Snorm},
    };

    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (const auto& c : cases) {
        GLuint texture = 0;
        MG_Impl::GLImpl::GenTextures(1, &texture);
        MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
        MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, c.internalFormat, 4, 4, 0, c.format, c.type, nullptr);

        const auto textureObject = MG_State::pGLContext->GetTextureObject(texture);
        ASSERT_NE(textureObject, nullptr) << "internalFormat 0x" << std::hex << c.internalFormat;
        EXPECT_EQ(textureObject->GetFormat(), c.expected) << "internalFormat 0x" << std::hex << c.internalFormat;
        EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR) << "internalFormat 0x" << std::hex << c.internalFormat;
    }
}

// RGTC compresses 4x4 blocks of a 2D image and has no 3D form, so glTexImage3D must reject it even
// though the same enum is accepted on a 2D target. The generic compressed formats carry no such
// restriction and stay legal in 3D.
TEST_F(TextureTest, RgtcInternalFormatsAreRejectedOnThreeDimensionalTargets) {
    const GLenum rgtc[] = {GL_COMPRESSED_RED_RGTC1, GL_COMPRESSED_SIGNED_RED_RGTC1, GL_COMPRESSED_RG_RGTC2,
                           GL_COMPRESSED_SIGNED_RG_RGTC2};
    for (const GLenum internalFormat : rgtc) {
        GLuint texture = 0;
        MG_Impl::GLImpl::GenTextures(1, &texture);
        MG_Impl::GLImpl::BindTexture(GL_TEXTURE_3D, texture);
        MG_Impl::GLImpl::TexImage3D(GL_TEXTURE_3D, 0, internalFormat, 4, 4, 4, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
        EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_OPERATION)
            << "internalFormat 0x" << std::hex << internalFormat;
    }

    GLuint generic = 0;
    MG_Impl::GLImpl::GenTextures(1, &generic);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_3D, generic);
    MG_Impl::GLImpl::TexImage3D(GL_TEXTURE_3D, 0, GL_COMPRESSED_RGBA, 4, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, TextureStorage3DAndSubImageModifyNamedObjectOnly) {
    GLuint texture = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_3D, 1, &texture);
    MG_Impl::GLImpl::TextureStorage3D(texture, 2, GL_R8, 2, 2, 2);

    const Uint8 pixels[] = {
        1, 2, 3, 4,
        5, 6, 7, 8,
    };
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    MG_Impl::GLImpl::TextureSubImage3D(texture, 0, 0, 0, 0, 2, 2, 2, GL_RED, GL_UNSIGNED_BYTE, pixels);

    const auto textureObject = MG_State::pGLContext->GetTextureObject(texture);
    auto* mipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
    ASSERT_NE(mipmapObject, nullptr);
    EXPECT_EQ(mipmapObject->GetMipmapTexelSize(TextureUploadTarget::Texture3D, 0), IntVec3(2, 2, 2));
    EXPECT_EQ(mipmapObject->GetMipmapTexelSize(TextureUploadTarget::Texture3D, 1), IntVec3(1, 1, 1));
    EXPECT_TRUE(mipmapObject->IsStorageDirty(TextureUploadTarget::Texture3D, 0));

    const auto* stored = static_cast<const Uint8*>(mipmapObject->MapMipmapData(TextureUploadTarget::Texture3D, 0));
    ASSERT_NE(stored, nullptr);
    EXPECT_EQ(std::memcmp(stored, pixels, sizeof(pixels)), 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

namespace {
    const Uint8* GetBoundTexture3DLevelBytes(GLuint texture, Uint level = 0) {
        const auto textureObject = MG_State::pGLContext->GetTextureObject(texture);
        auto* mipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
        return static_cast<const Uint8*>(mipmapObject->MapMipmapData(TextureUploadTarget::Texture3D, level));
    }
} // namespace

TEST_F(TextureTest, BoundTexImage3DUnsizedRgbaInfersRgba8AndUnpacksBgra8888Rev) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_3D, texture);

    const Uint8 pixels[] = {
        10, 20, 30, 40,
        50, 60, 70, 80,
        90, 100, 110, 120,
        130, 140, 150, 160,
    };
    MG_Impl::GLImpl::TexImage3D(GL_TEXTURE_3D, 0, GL_RGBA, 2, 1, 2, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, pixels);

    const auto textureObject = MG_State::pGLContext->GetTextureObject(texture);
    EXPECT_EQ(textureObject->GetFormat(), TextureInternalFormat::RGBA8);

    const auto* stored = GetBoundTexture3DLevelBytes(texture);
    ASSERT_NE(stored, nullptr);
    const Uint8 expected[] = {
        30, 20, 10, 40,
        70, 60, 50, 80,
        110, 100, 90, 120,
        150, 140, 130, 160,
    };
    for (SizeT i = 0; i < sizeof(expected); ++i) {
        EXPECT_EQ(stored[i], expected[i]) << "byte " << i;
    }
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, BoundTexImage3DHonorsImageHeightAndSkipImages) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_3D, texture);

    // Source cuboid is 2x3 per image (IMAGE_HEIGHT = 3) with one leading image skipped;
    // the upload reads a 2x2x2 sub-cuboid.
    const Uint8 pixels[] = {
        // image 0 (skipped)
        0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
        0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
        0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
        // image 1: rows 0-1 are slice 0, row 2 is padding
        1, 2, 3, 4, 5, 6, 7, 8,
        9, 10, 11, 12, 13, 14, 15, 16,
        0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB,
        // image 2: rows 0-1 are slice 1, row 2 is padding
        17, 18, 19, 20, 21, 22, 23, 24,
        25, 26, 27, 28, 29, 30, 31, 32,
        0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB,
    };

    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_IMAGE_HEIGHT, 3);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_SKIP_IMAGES, 1);
    MG_Impl::GLImpl::TexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, 2, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_SKIP_IMAGES, 0);

    const auto* stored = GetBoundTexture3DLevelBytes(texture);
    ASSERT_NE(stored, nullptr);
    const Uint8 expected[] = {
        1, 2, 3, 4, 5, 6, 7, 8,
        9, 10, 11, 12, 13, 14, 15, 16,
        17, 18, 19, 20, 21, 22, 23, 24,
        25, 26, 27, 28, 29, 30, 31, 32,
    };
    for (SizeT i = 0; i < sizeof(expected); ++i) {
        EXPECT_EQ(stored[i], expected[i]) << "byte " << i;
    }
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, BoundTexImage3DConvertsRedToRgba8WithImageHeightAndSkips) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_3D, texture);

    // Source cuboid: ROW_LENGTH = 3 (1-byte texels, alignment 1), IMAGE_HEIGHT = 2,
    // skip 1 image, 0 rows, 1 pixel; upload a 2x1x2 sub-cuboid of GL_RED texels.
    const Uint8 pixels[] = {
        // image 0 (skipped)
        90, 91, 92,
        93, 94, 95,
        // image 1: row 0 holds slice 0 at x offset 1, row 1 is padding
        80, 11, 12,
        81, 82, 83,
        // image 2: row 0 holds slice 1 at x offset 1, row 1 is padding
        84, 21, 22,
        85, 86, 87,
    };

    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_ROW_LENGTH, 3);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_SKIP_PIXELS, 1);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_IMAGE_HEIGHT, 2);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_SKIP_IMAGES, 1);
    MG_Impl::GLImpl::TexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, 2, 1, 2, 0, GL_RED, GL_UNSIGNED_BYTE, pixels);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_SKIP_IMAGES, 0);

    const auto* stored = GetBoundTexture3DLevelBytes(texture);
    ASSERT_NE(stored, nullptr);
    const Uint8 expected[] = {
        11, 0, 0, 255, 12, 0, 0, 255,
        21, 0, 0, 255, 22, 0, 0, 255,
    };
    for (SizeT i = 0; i < sizeof(expected); ++i) {
        EXPECT_EQ(stored[i], expected[i]) << "byte " << i;
    }
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, BoundTexSubImage3DUnpacksPackedBgra8888RevIntoCorrectSlice) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_3D, texture);

    const Uint8 zeros[2 * 2 * 2 * 4] = {};
    MG_Impl::GLImpl::TexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, 2, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, zeros);

    const Uint8 pixels[] = {10, 20, 30, 40};
    MG_Impl::GLImpl::TexSubImage3D(GL_TEXTURE_3D, 0, 1, 1, 1, 1, 1, 1, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, pixels);

    const auto* stored = GetBoundTexture3DLevelBytes(texture);
    ASSERT_NE(stored, nullptr);
    Uint8 expected[2 * 2 * 2 * 4] = {};
    expected[28] = 30;
    expected[29] = 20;
    expected[30] = 10;
    expected[31] = 40;
    for (SizeT i = 0; i < sizeof(expected); ++i) {
        EXPECT_EQ(stored[i], expected[i]) << "byte " << i;
    }
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, BoundTexSubImage3DRejectsOutOfRangeLevel) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_3D, texture);

    const Uint8 zeros[2 * 2 * 2 * 4] = {};
    MG_Impl::GLImpl::TexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, 2, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, zeros);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    const Uint8 pixels[] = {1, 2, 3, 4};
    MG_Impl::GLImpl::TexSubImage3D(GL_TEXTURE_3D, 3, 0, 0, 0, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_VALUE);
}

// The GL CTS KHR-GL33.pixelstoragemodes.teximage3d cases upload GL_TEXTURE_2D_ARRAY
// textures through glTexImage3D with UNPACK_ROW_LENGTH / IMAGE_HEIGHT / SKIP_* set to
// extract a sub-cuboid; this mirrors that shape (scaled down) on the 2D-array target.
TEST_F(TextureTest, BoundTexImage3DOn2DArrayHonorsUnpackSubcuboidSelection) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D_ARRAY, texture);

    // Source cuboid: 3x3 RGBA texels per image, 3 images; skip 1 image, 1 row, 1 pixel;
    // upload the 2x2x2 sub-cuboid. Each source byte equals its own offset, so the stored
    // shadow bytes must equal the offsets of the selected texels.
    Uint8 pixels[3 * 3 * 3 * 4];
    for (SizeT i = 0; i < sizeof(pixels); ++i) {
        pixels[i] = static_cast<Uint8>(i);
    }

    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_ROW_LENGTH, 3);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_IMAGE_HEIGHT, 3);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_SKIP_PIXELS, 1);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_SKIP_ROWS, 1);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_SKIP_IMAGES, 1);
    MG_Impl::GLImpl::TexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 2, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_SKIP_IMAGES, 0);

    const auto textureObject = MG_State::pGLContext->GetTextureObject(texture);
    ASSERT_NE(textureObject, nullptr);
    EXPECT_EQ(textureObject->GetTarget(), TextureTarget::Texture2DArray);
    auto* mipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
    EXPECT_EQ(mipmapObject->GetMipmapTexelSize(TextureUploadTarget::Texture2DArray, 0), IntVec3(2, 2, 2));

    const auto* stored =
        static_cast<const Uint8*>(mipmapObject->MapMipmapData(TextureUploadTarget::Texture2DArray, 0));
    ASSERT_NE(stored, nullptr);
    SizeT storedIndex = 0;
    for (SizeT image = 1; image <= 2; ++image) {         // SKIP_IMAGES = 1
        for (SizeT row = 1; row <= 2; ++row) {            // SKIP_ROWS = 1
            for (SizeT column = 1; column <= 2; ++column) { // SKIP_PIXELS = 1
                const SizeT srcOffset = image * 36 + row * 12 + column * 4;
                for (SizeT b = 0; b < 4; ++b, ++storedIndex) {
                    EXPECT_EQ(stored[storedIndex], static_cast<Uint8>(srcOffset + b)) << "byte " << storedIndex;
                }
            }
        }
    }
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// The shadow mip for packed sized formats keeps the client's packed bytes, so the
// canonical transfer triple must name the packed word type; the old default fallback
// (GL_UNSIGNED_BYTE) made backends read 4 bytes per texel from a 2-byte-per-texel
// shadow (KHR-GL33.pixelstoragemodes rgba4/rgb565 uploads), and GL_RGB10_A2UI got a
// non-integer GL_RGB transfer format the driver rejects outright.
TEST_F(TextureTest, NormalizePixelFormatKeepsPackedTransferTypesForPackedSizedFormats) {
    using MG_Util::TextureFormatProcessor::NormalizePixelFormat;
    struct {
        GLenum internalFormat;
        GLenum expectedFormat;
        GLenum expectedType;
    } cases[] = {
        // RGBA4/RGB565/RGB5_A1 store canonical UNorm8 component shadows (PixelStoreProcessor
        // GetInternalShadowLayout), so their transfer type is GL_UNSIGNED_BYTE; the 32-bit packed
        // formats keep the packed word the shadow holds verbatim.
        {GL_RGBA4, GL_RGBA, GL_UNSIGNED_BYTE},
        {GL_RGB565, GL_RGB, GL_UNSIGNED_BYTE},
        {GL_RGB10_A2UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT_2_10_10_10_REV},
        {GL_RGB5_A1, GL_RGBA, GL_UNSIGNED_BYTE},
        {GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV},
    };
    for (const auto& c : cases) {
        GLenum outInternal = 0, outFormat = 0, outType = 0;
        NormalizePixelFormat(c.internalFormat, PixelFormatNormalizeOptionBit::None, &outInternal, &outFormat,
                             &outType);
        EXPECT_EQ(outInternal, c.internalFormat) << "internalformat 0x" << std::hex << c.internalFormat;
        EXPECT_EQ(outFormat, c.expectedFormat) << "internalformat 0x" << std::hex << c.internalFormat;
        EXPECT_EQ(outType, c.expectedType) << "internalformat 0x" << std::hex << c.internalFormat;
    }
}

// GL_RGB565 (ARB_ES2_compatibility / GL 4.1, used directly by the GL CTS) must round-trip
// through the internal-format enums; it had no GLToMG mapping at all, so glTexImage* with
// GL_RGB565 was rejected as an unknown internal format.
TEST_F(TextureTest, Rgb565InternalFormatRoundTripsThroughEnumConverters) {
    EXPECT_EQ(MG_Util::ConvertGLEnumToTextureInternalFormat(GL_RGB565), TextureInternalFormat::RGB5);
    EXPECT_EQ(MG_Util::ConvertGLEnumToTextureInternalFormat(GL_RGB5), TextureInternalFormat::RGB5);
    // The ES-facing rendition of RGB5 is GL_RGB565 (desktop GL_RGB5 is not a legal sized
    // internalformat on OpenGL ES backends).
    EXPECT_EQ(MG_Util::ConvertTextureInternalFormatToGLEnum(TextureInternalFormat::RGB5),
              static_cast<GLenum>(GL_RGB565));
}

// Regression guard: the DirectGLES backend must treat GL_TEXTURE_2D_ARRAY as a
// syncable target — it used to be skipped entirely, so 2D-array textures were never
// uploaded or bound (KHR-GL33.pixelstoragemodes.teximage3d.* failed wholesale).
TEST_F(TextureTest, DirectGLESTreats2DArrayAsSupportedTextureTarget) {
    using MobileGL::MG_Backend::DirectGLES::TextureImpl::IsSupportedTextureTarget;
    EXPECT_TRUE(IsSupportedTextureTarget(TextureTarget::Texture2DArray));
    EXPECT_TRUE(IsSupportedTextureTarget(TextureTarget::Texture3D));
    EXPECT_TRUE(IsSupportedTextureTarget(TextureTarget::Texture2D));
    // Every desktop-only target is stored on an ES one (MapToBackendTextureTarget): 1D and
    // 1D-array as 2D / 2D-array, matching SPIRV-Cross's ES 1D-as-2D shader emission, and
    // rectangle as a plain 2D - it is single-level and already clamps, so only the
    // non-normalized coordinates differ and LowerRectImages handles those.
    EXPECT_TRUE(IsSupportedTextureTarget(TextureTarget::Texture1D));
    EXPECT_TRUE(IsSupportedTextureTarget(TextureTarget::Texture1DArray));
    EXPECT_TRUE(IsSupportedTextureTarget(TextureTarget::TextureRectangle));
}

// 2D-array textures keep their layer count constant across mip levels (GL 3.3 §3.9);
// only true 3D textures halve depth per level.
TEST_F(TextureTest, TexStorage3DOn2DArrayKeepsLayerCountAcrossLevels) {
    GLuint arrayTexture = 0;
    MG_Impl::GLImpl::GenTextures(1, &arrayTexture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D_ARRAY, arrayTexture);
    MG_Impl::GLImpl::TexStorage3D(GL_TEXTURE_2D_ARRAY, 3, GL_RGBA8, 8, 8, 4);

    const auto arrayObject = MG_State::pGLContext->GetTextureObject(arrayTexture);
    ASSERT_NE(arrayObject, nullptr);
    auto* arrayMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(arrayObject.get());
    EXPECT_EQ(arrayMipmapObject->GetMipmapTexelSize(TextureUploadTarget::Texture2DArray, 0), IntVec3(8, 8, 4));
    EXPECT_EQ(arrayMipmapObject->GetMipmapTexelSize(TextureUploadTarget::Texture2DArray, 1), IntVec3(4, 4, 4));
    EXPECT_EQ(arrayMipmapObject->GetMipmapTexelSize(TextureUploadTarget::Texture2DArray, 2), IntVec3(2, 2, 4));
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // Control: a real 3D texture still halves its depth per level.
    GLuint volumeTexture = 0;
    MG_Impl::GLImpl::GenTextures(1, &volumeTexture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_3D, volumeTexture);
    MG_Impl::GLImpl::TexStorage3D(GL_TEXTURE_3D, 3, GL_RGBA8, 8, 8, 4);

    const auto volumeObject = MG_State::pGLContext->GetTextureObject(volumeTexture);
    ASSERT_NE(volumeObject, nullptr);
    auto* volumeMipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(volumeObject.get());
    EXPECT_EQ(volumeMipmapObject->GetMipmapTexelSize(TextureUploadTarget::Texture3D, 0), IntVec3(8, 8, 4));
    EXPECT_EQ(volumeMipmapObject->GetMipmapTexelSize(TextureUploadTarget::Texture3D, 1), IntVec3(4, 4, 2));
    EXPECT_EQ(volumeMipmapObject->GetMipmapTexelSize(TextureUploadTarget::Texture3D, 2), IntVec3(2, 2, 1));
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, NamedTextureVectorParametersAndGettersWorkWithoutBinding) {
    GLuint texture = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &texture);

    const GLfloat borderColor[] = {0.25f, 0.5f, 0.75f, 1.0f};
    const GLint swizzle[] = {GL_BLUE, GL_GREEN, GL_RED, GL_ALPHA};
    MG_Impl::GLImpl::TextureParameterfv(texture, GL_TEXTURE_BORDER_COLOR, borderColor);
    MG_Impl::GLImpl::TextureParameterIiv(texture, GL_TEXTURE_SWIZZLE_RGBA, swizzle);

    GLfloat reportedBorder[4] = {};
    GLint reportedSwizzle[4] = {};
    MG_Impl::GLImpl::GetTextureParameterfv(texture, GL_TEXTURE_BORDER_COLOR, reportedBorder);
    MG_Impl::GLImpl::GetTextureParameterIiv(texture, GL_TEXTURE_SWIZZLE_RGBA, reportedSwizzle);

    EXPECT_FLOAT_EQ(reportedBorder[0], borderColor[0]);
    EXPECT_FLOAT_EQ(reportedBorder[1], borderColor[1]);
    EXPECT_FLOAT_EQ(reportedBorder[2], borderColor[2]);
    EXPECT_FLOAT_EQ(reportedBorder[3], borderColor[3]);
    EXPECT_EQ(reportedSwizzle[0], GL_BLUE);
    EXPECT_EQ(reportedSwizzle[1], GL_GREEN);
    EXPECT_EQ(reportedSwizzle[2], GL_RED);
    EXPECT_EQ(reportedSwizzle[3], GL_ALPHA);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, GetInternalformativReportsBasicTextureMetadata) {
    ScopedBackendOverride backend(MakeUnique<FormatCapabilityBackend>());
    GLint params[4] = {};

    MG_Impl::GLImpl::GetInternalformativ(GL_TEXTURE_2D, GL_RGBA8, GL_INTERNALFORMAT_SUPPORTED, 1, params);
    EXPECT_EQ(params[0], GL_TRUE);

    MG_Impl::GLImpl::GetInternalformativ(GL_TEXTURE_2D, GL_RGBA8, GL_FRAMEBUFFER_RENDERABLE, 1, params);
    EXPECT_EQ(params[0], GL_FULL_SUPPORT);

    MG_Impl::GLImpl::GetInternalformativ(GL_TEXTURE_2D, GL_RGBA8, GL_FILTER, 1, params);
    EXPECT_EQ(params[0], GL_FULL_SUPPORT);

    MG_Impl::GLImpl::GetInternalformativ(GL_TEXTURE_2D, GL_RGBA8, GL_INTERNALFORMAT_RED_SIZE, 1, params);
    EXPECT_EQ(params[0], 8);

    MG_Impl::GLImpl::GetInternalformativ(GL_TEXTURE_2D, GL_RGBA8, GL_INTERNALFORMAT_RED_TYPE, 1, params);
    EXPECT_EQ(params[0], GL_UNSIGNED_NORMALIZED);

    MG_Impl::GLImpl::GetInternalformativ(GL_TEXTURE_2D, GL_RGBA8, GL_TEXTURE_IMAGE_FORMAT, 1, params);
    EXPECT_EQ(params[0], GL_RGBA);

    MG_Impl::GLImpl::GetInternalformativ(GL_TEXTURE_2D, GL_RGBA8, GL_TEXTURE_IMAGE_TYPE, 1, params);
    EXPECT_EQ(params[0], GL_UNSIGNED_BYTE);

    MG_Impl::GLImpl::GetInternalformativ(GL_TEXTURE_2D, GL_RG8, GL_INTERNALFORMAT_SUPPORTED, 1, params);
    EXPECT_EQ(params[0], GL_TRUE);

    MG_Impl::GLImpl::GetInternalformativ(GL_TEXTURE_2D, GL_RG8, GL_FILTER, 1, params);
    EXPECT_EQ(params[0], GL_CAVEAT_SUPPORT);

    MG_Impl::GLImpl::GetInternalformativ(GL_TEXTURE_3D, GL_DEPTH24_STENCIL8, GL_FRAMEBUFFER_RENDERABLE_LAYERED, 1,
                                         params);
    EXPECT_EQ(params[0], GL_FULL_SUPPORT);

    MG_Impl::GLImpl::GetInternalformativ(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, GL_NUM_SAMPLE_COUNTS, 1, params);
    EXPECT_EQ(params[0], 3);

    MG_Impl::GLImpl::GetInternalformativ(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, GL_SAMPLES, 4, params);
    EXPECT_EQ(params[0], 4);
    EXPECT_EQ(params[1], 2);
    EXPECT_EQ(params[2], 1);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, BoundTexImage2DExpandsRedUnsignedByteToRgba8) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);

    const Uint8 pixels[] = {
        10, 20,
        30, 40,
    };
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RED, GL_UNSIGNED_BYTE, pixels);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_ALIGNMENT, 4);

    const auto* stored = GetBoundTexture2DLevelBytes(texture);
    const Uint8 expected[] = {
        10, 0, 0, 255,
        20, 0, 0, 255,
        30, 0, 0, 255,
        40, 0, 0, 255,
    };
    for (SizeT i = 0; i < sizeof(expected); ++i) {
        EXPECT_EQ(stored[i], expected[i]) << "byte " << i;
    }
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, BoundTexSubImage2DExpandsRgUnsignedByteToRgba8) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    const Uint8 pixels[] = {
        10, 20,
        30, 40,
    };
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    MG_Impl::GLImpl::TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 2, 1, GL_RG, GL_UNSIGNED_BYTE, pixels);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_ALIGNMENT, 4);

    const auto* stored = GetBoundTexture2DLevelBytes(texture);
    const Uint8 expected[] = {
        10, 20, 0, 255,
        30, 40, 0, 255,
    };
    for (SizeT i = 0; i < sizeof(expected); ++i) {
        EXPECT_EQ(stored[i], expected[i]) << "byte " << i;
    }
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, BoundTexImage2DReordersBgrUnsignedByteToRgba8) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);

    const Uint8 pixels[] = {
        1, 2, 3,
        4, 5, 6,
    };
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 1, 0, GL_BGR, GL_UNSIGNED_BYTE, pixels);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_ALIGNMENT, 4);

    const auto* stored = GetBoundTexture2DLevelBytes(texture);
    const Uint8 expected[] = {
        3, 2, 1, 255,
        6, 5, 4, 255,
    };
    for (SizeT i = 0; i < sizeof(expected); ++i) {
        EXPECT_EQ(stored[i], expected[i]) << "byte " << i;
    }
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, BoundTexImage2DConvertsRedFloatToRgba8) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);

    const GLfloat pixels[] = {
        0.0f, 0.5f,
        1.0f, 2.0f, // out-of-range values clamp to [0, 1]
    };
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RED, GL_FLOAT, pixels);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_ALIGNMENT, 4);

    const auto* stored = GetBoundTexture2DLevelBytes(texture);
    const Uint8 expected[] = {
        0, 0, 0, 255,
        128, 0, 0, 255,
        255, 0, 0, 255,
        255, 0, 0, 255,
    };
    for (SizeT i = 0; i < sizeof(expected); ++i) {
        EXPECT_EQ(stored[i], expected[i]) << "byte " << i;
    }
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, BoundTexImage2DExpandsRedIntegerUnsignedShortToRgba8ui) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);

    const Uint16 pixels[] = {
        10, 300, // 300 exceeds the 8-bit destination and clamps to 255
    };
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8UI, 2, 1, 0, GL_RED_INTEGER, GL_UNSIGNED_SHORT, pixels);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_ALIGNMENT, 4);

    const auto* stored = GetBoundTexture2DLevelBytes(texture);
    const Uint8 expected[] = {
        10, 0, 0, 1, // integer formats default missing alpha to 1, not the type maximum
        255, 0, 0, 1,
    };
    for (SizeT i = 0; i < sizeof(expected); ++i) {
        EXPECT_EQ(stored[i], expected[i]) << "byte " << i;
    }
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, BoundTexImage2DExpandsRedToRgba8WithRowLengthAndSkips) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);

    const Uint8 pixels[] = {
        1, 2, 3, 4,
        5, 6, 7, 8,
        9, 10, 11, 12,
    };
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_ROW_LENGTH, 4);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_SKIP_PIXELS, 1);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_SKIP_ROWS, 1);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RED, GL_UNSIGNED_BYTE, pixels);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_ALIGNMENT, 4);

    const auto* stored = GetBoundTexture2DLevelBytes(texture);
    const Uint8 expected[] = {
        6, 0, 0, 255,
        7, 0, 0, 255,
        10, 0, 0, 255,
        11, 0, 0, 255,
    };
    for (SizeT i = 0; i < sizeof(expected); ++i) {
        EXPECT_EQ(stored[i], expected[i]) << "byte " << i;
    }
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, NormalizeDepth24Stencil8UsesPackedDepthStencilType) {
    GLenum internalFormat = 0;
    GLenum format = 0;
    GLenum type = 0;
    MG_Util::TextureFormatProcessor::NormalizePixelFormat(GL_DEPTH24_STENCIL8,
                                                          PixelFormatNormalizeOptionBit::None,
                                                          &internalFormat, &format, &type);

    EXPECT_EQ(internalFormat, GL_DEPTH24_STENCIL8);
    EXPECT_EQ(format, GL_DEPTH_STENCIL);
    EXPECT_EQ(type, GL_UNSIGNED_INT_24_8);
}

// ==================== Default texture objects (name 0), GL 3.3 core 3.8 ====================

TEST_F(TextureTest, DefaultTextureIsBoundInitiallyAndIsPerTarget) {
    // The initial binding of every unit/target slot is the target's default texture object.
    const auto& default2D = MG_State::pGLContext->GetDefaultTextureObject(TextureTarget::Texture2D);
    const auto& default3D = MG_State::pGLContext->GetDefaultTextureObject(TextureTarget::Texture3D);
    ASSERT_NE(default2D, nullptr);
    ASSERT_NE(default3D, nullptr);
    EXPECT_NE(default2D, default3D);
    EXPECT_EQ(default2D->GetExternalIndex(), 0u);
    EXPECT_EQ(default2D->GetTarget(), TextureTarget::Texture2D);
    EXPECT_EQ(default3D->GetTarget(), TextureTarget::Texture3D);

    // Binding 0 restores the default object, and the binding query reports name 0.
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, 0);
    EXPECT_EQ(MG_State::pGLContext->GetTextureUnitObject(0)
                  .GetBindingSlot(TextureTarget::Texture2D)
                  .GetBoundObject(),
              default2D);
    GLint binding = -1;
    MG_Impl::GLImpl::GetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
    EXPECT_EQ(binding, 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // One default per target per context, shared across all texture units.
    EXPECT_EQ(MG_State::pGLContext->GetTextureUnitObject(5)
                  .GetBindingSlot(TextureTarget::Texture2D)
                  .GetBoundObject(),
              default2D);
}

// Backend sampled-set membership keys off IsUndefinedDefaultTexture, so a default texture
// crossing the Unknown<->defined boundary must move the bind generation even though no bind
// happened - a cached sampled set (DirectVulkan walk-skip) would otherwise replay stale
// membership and never sync/transition the now-image-bearing default. Positioned while the 2D
// default is still undefined in a single-process run (later tests define it and definedness is
// irreversible through the GL API); the reverse transition at the end restores that state.
TEST_F(TextureTest, DefiningImageOnBoundDefaultTextureBumpsBindGeneration) {
    MG_Impl::GLImpl::ActiveTexture(GL_TEXTURE0);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, 0);
    const auto& defaultTexture =
        MG_State::pGLContext->GetTextureUnitObject(0).GetBindingSlot(TextureTarget::Texture2D).GetBoundObject();
    ASSERT_TRUE(MG_State::GLState::IsUndefinedDefaultTexture(defaultTexture.get()))
        << "an earlier test defined the 2D default texture; move this test before it";

    // glTexImage2D on the BOUND name-0 texture (no rebind anywhere) moves the generation
    // exactly once: the next draw re-collects the sampled set and references the default's
    // image instead of the fallback.
    const Uint64 base = MG_State::pGLContext->GetTextureBindGeneration();
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_EQ(MG_State::pGLContext->GetTextureBindGeneration(), base + 1);

    // Re-specifying an already-defined default keeps the cache hot.
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_EQ(MG_State::pGLContext->GetTextureBindGeneration(), base + 1);

    // Other externalIndex-0 objects (proxy textures, default-FBO attachments) are not the
    // context's default texture; their (re)specification must not churn the cache.
    auto proxyLike = MakeShared<MG_State::GLState::TextureObject2D>(0u);
    proxyLike->SetInternalFormat(TextureInternalFormat::RGBA8);
    EXPECT_EQ(MG_State::pGLContext->GetTextureBindGeneration(), base + 1);

    // The reverse transition (no GL entry point produces it today) is symmetric, and restores
    // the undefined 2D default the rest of the suite expects.
    defaultTexture->SetInternalFormat(TextureInternalFormat::Unknown);
    EXPECT_EQ(MG_State::pGLContext->GetTextureBindGeneration(), base + 2);
    EXPECT_TRUE(MG_State::GLState::IsUndefinedDefaultTexture(defaultTexture.get()));
}

TEST_F(TextureTest, DefaultTextureAcceptsImageAndParameterCalls) {
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, 0);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // The exact shape of GL CTS's per-case state reset (gluStateReset resetStateGLCore): a
    // zero-sized TexImage2D plus parameter resets on the default texture, all of which must
    // succeed without recording anything.
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    GLint minFilter = 0;
    MG_Impl::GLImpl::GetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &minFilter);
    EXPECT_EQ(minFilter, GL_NEAREST);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // A real upload works like on any texture: data lands in the default object's shadow store.
    const Uint8 pixels[] = {
        1, 2, 3, 4,
        5, 6, 7, 8,
    };
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    const auto& default2D = MG_State::pGLContext->GetDefaultTextureObject(TextureTarget::Texture2D);
    auto* mipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(default2D.get());
    EXPECT_EQ(mipmapObject->GetMipmapTexelSize(TextureUploadTarget::Texture2D, 0), IntVec3(2, 1, 1));
    const auto* stored =
        static_cast<const Uint8*>(mipmapObject->MapMipmapData(TextureUploadTarget::Texture2D, 0));
    ASSERT_NE(stored, nullptr);
    EXPECT_EQ(std::memcmp(stored, pixels, sizeof(pixels)), 0);

    // Restore the CTS-reset shape (zero-sized level 0, default parameters) so later tests see
    // the default texture in its usual post-reset state regardless of execution order.
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    MG_Impl::GLImpl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, DefaultTextureParametersAreSharedAcrossUnits) {
    MG_Impl::GLImpl::ActiveTexture(GL_TEXTURE0);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, 0);
    MG_Impl::GLImpl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // The same default object is bound on every unit, so the parameter shows up on unit 1 too.
    MG_Impl::GLImpl::ActiveTexture(GL_TEXTURE1);
    GLint wrapS = 0;
    MG_Impl::GLImpl::GetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &wrapS);
    EXPECT_EQ(wrapS, GL_CLAMP_TO_EDGE);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // The 3D default is a distinct object and keeps its own (initial) wrap mode.
    GLint wrapS3D = 0;
    MG_Impl::GLImpl::GetTexParameteriv(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, &wrapS3D);
    EXPECT_EQ(wrapS3D, GL_REPEAT);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    MG_Impl::GLImpl::ActiveTexture(GL_TEXTURE0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, DefaultTextureIsNotAnObjectNameAndSurvivesDeleteCalls) {
    // glIsTexture(0) is GL_FALSE (name 0 is never a GenTextures name), with no error.
    EXPECT_EQ(MG_Impl::GLImpl::IsTexture(0), GL_FALSE);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_FALSE(MG_State::pGLContext->ValidateTextureObject(0));
    EXPECT_FALSE(MG_State::pGLContext->ValidateTextureName(0));

    // Deleting name 0 is silently ignored and leaves the default object fully usable.
    constexpr GLuint zero = 0;
    MG_Impl::GLImpl::DeleteTextures(1, &zero);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_NE(MG_State::pGLContext->GetDefaultTextureObject(TextureTarget::Texture2D), nullptr);

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, 0);
    MG_Impl::GLImpl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, DeletingBoundTextureRebindsDefaultTexture) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::DeleteTextures(1, &texture);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // GL 3.3 core 3.8.1: deleting the bound texture is as if BindTexture(target, 0) had run.
    EXPECT_EQ(MG_State::pGLContext->GetTextureUnitObject(0)
                  .GetBindingSlot(TextureTarget::Texture2D)
                  .GetBoundObject(),
              MG_State::pGLContext->GetDefaultTextureObject(TextureTarget::Texture2D));
    GLint binding = -1;
    MG_Impl::GLImpl::GetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
    EXPECT_EQ(binding, 0);

    // Image and parameter calls keep working against the (now bound) default texture.
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, NamedTextureRebindsAndWorksAfterUsingDefault) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
    const Uint8 pixels[] = {9, 8, 7, 6};
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // Detour through the default texture, then rebind the named one.
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, 0);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);

    GLint binding = -1;
    MG_Impl::GLImpl::GetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
    EXPECT_EQ(binding, static_cast<GLint>(texture));

    // The named texture's contents were not disturbed by the operations on the default.
    const auto* stored = GetBoundTexture2DLevelBytes(texture);
    ASSERT_NE(stored, nullptr);
    EXPECT_EQ(std::memcmp(stored, pixels, sizeof(pixels)), 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::DeleteTextures(1, &texture);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, TexStorageOnDefaultTextureIsInvalidOperation) {
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, 0);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // ARB_texture_storage: "An INVALID_OPERATION error is generated if zero is bound to target"
    // - immutable storage can never be established on a default texture.
    MG_Impl::GLImpl::TexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 2, 2);
    ExpectSingleGlError(GL_INVALID_OPERATION);
    EXPECT_FALSE(MG_State::pGLContext->GetDefaultTextureObject(TextureTarget::Texture2D)->IsImmutable());
}

TEST_F(TextureTest, CtsStyleStateResetOnDefaultTexturesLeavesNoError) {
    // Mirrors the texture section of VK-GL-CTS gluStateReset resetStateGLCore, which runs after
    // EVERY case: bind 0 on each target, clear the default texture's image with a zero-sized
    // TexImage*, and reset sampler-ish parameters. Any leftover error aborts the whole batch
    // ("Texture state reset failed"), so this exact sequence must stay clean end to end.
    const GLfloat borderColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const auto resetCommonTexParams = [&borderColor](GLenum target) {
        MG_Impl::GLImpl::TexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR);
        MG_Impl::GLImpl::TexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        MG_Impl::GLImpl::TexParameterfv(target, GL_TEXTURE_BORDER_COLOR, borderColor);
        MG_Impl::GLImpl::TexParameteri(target, GL_TEXTURE_WRAP_S, GL_REPEAT);
        MG_Impl::GLImpl::TexParameteri(target, GL_TEXTURE_WRAP_T, GL_REPEAT);
        MG_Impl::GLImpl::TexParameterf(target, GL_TEXTURE_MIN_LOD, -1000.0f);
        MG_Impl::GLImpl::TexParameterf(target, GL_TEXTURE_MAX_LOD, 1000.0f);
        MG_Impl::GLImpl::TexParameteri(target, GL_TEXTURE_BASE_LEVEL, 0);
        MG_Impl::GLImpl::TexParameteri(target, GL_TEXTURE_MAX_LEVEL, 1000);
        MG_Impl::GLImpl::TexParameterf(target, GL_TEXTURE_LOD_BIAS, 0.0f);
        MG_Impl::GLImpl::TexParameteri(target, GL_TEXTURE_COMPARE_MODE, GL_NONE);
        MG_Impl::GLImpl::TexParameteri(target, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
        MG_Impl::GLImpl::TexParameteri(target, GL_TEXTURE_SWIZZLE_R, GL_RED);
        MG_Impl::GLImpl::TexParameteri(target, GL_TEXTURE_SWIZZLE_G, GL_GREEN);
        MG_Impl::GLImpl::TexParameteri(target, GL_TEXTURE_SWIZZLE_B, GL_BLUE);
        MG_Impl::GLImpl::TexParameteri(target, GL_TEXTURE_SWIZZLE_A, GL_ALPHA);
    };

    MG_Impl::GLImpl::ActiveTexture(GL_TEXTURE0);

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_1D, 0);
    MG_Impl::GLImpl::TexImage1D(GL_TEXTURE_1D, 0, GL_RGBA, 0, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    resetCommonTexParams(GL_TEXTURE_1D);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR) << "GL_TEXTURE_1D reset failed";

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, 0);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    resetCommonTexParams(GL_TEXTURE_2D);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR) << "GL_TEXTURE_2D reset failed";

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_CUBE_MAP, 0);
    for (int face = 0; face < 6; ++face) {
        MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_RGBA, 0, 0, 0, GL_RGBA,
                                    GL_UNSIGNED_BYTE, nullptr);
    }
    resetCommonTexParams(GL_TEXTURE_CUBE_MAP);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR) << "GL_TEXTURE_CUBE_MAP reset failed";

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D_ARRAY, 0);
    MG_Impl::GLImpl::TexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA, 0, 0, 0, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                                nullptr);
    resetCommonTexParams(GL_TEXTURE_2D_ARRAY);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR) << "GL_TEXTURE_2D_ARRAY reset failed";

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_3D, 0);
    MG_Impl::GLImpl::TexImage3D(GL_TEXTURE_3D, 0, GL_RGBA, 0, 0, 0, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    MG_Impl::GLImpl::TexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_REPEAT);
    resetCommonTexParams(GL_TEXTURE_3D);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR) << "GL_TEXTURE_3D reset failed";

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_1D_ARRAY, 0);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_1D_ARRAY, 0, GL_RGBA, 0, 0, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                                nullptr);
    resetCommonTexParams(GL_TEXTURE_1D_ARRAY);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR) << "GL_TEXTURE_1D_ARRAY reset failed";

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_RECTANGLE, 0);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_RECTANGLE, 0, GL_RGBA, 0, 0, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                                nullptr);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR) << "GL_TEXTURE_RECTANGLE reset failed";

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_BUFFER, 0);
    MG_Impl::GLImpl::TexBuffer(GL_TEXTURE_BUFFER, GL_R8, 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR) << "GL_TEXTURE_BUFFER reset failed";

    // 3.2-core section: multisample defaults are cleared with ZERO-sized (and, for the array
    // target, zero-layer) TexImage*Multisample calls - GL only rejects negative dimensions.
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
    MG_Impl::GLImpl::TexParameteri(GL_TEXTURE_2D_MULTISAMPLE, GL_TEXTURE_SWIZZLE_R, GL_RED);
    MG_Impl::GLImpl::TexParameteri(GL_TEXTURE_2D_MULTISAMPLE, GL_TEXTURE_SWIZZLE_G, GL_GREEN);
    MG_Impl::GLImpl::TexParameteri(GL_TEXTURE_2D_MULTISAMPLE, GL_TEXTURE_SWIZZLE_B, GL_BLUE);
    MG_Impl::GLImpl::TexParameteri(GL_TEXTURE_2D_MULTISAMPLE, GL_TEXTURE_SWIZZLE_A, GL_ALPHA);
    MG_Impl::GLImpl::TexParameteri(GL_TEXTURE_2D_MULTISAMPLE, GL_TEXTURE_BASE_LEVEL, 0);
    MG_Impl::GLImpl::TexParameteri(GL_TEXTURE_2D_MULTISAMPLE, GL_TEXTURE_MAX_LEVEL, 1000);
    MG_Impl::GLImpl::TexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, 1, GL_RGBA8, 0, 0, GL_TRUE);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR) << "GL_TEXTURE_2D_MULTISAMPLE reset failed";

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D_MULTISAMPLE_ARRAY, 0);
    MG_Impl::GLImpl::TexParameteri(GL_TEXTURE_2D_MULTISAMPLE_ARRAY, GL_TEXTURE_SWIZZLE_R, GL_RED);
    MG_Impl::GLImpl::TexParameteri(GL_TEXTURE_2D_MULTISAMPLE_ARRAY, GL_TEXTURE_SWIZZLE_G, GL_GREEN);
    MG_Impl::GLImpl::TexParameteri(GL_TEXTURE_2D_MULTISAMPLE_ARRAY, GL_TEXTURE_SWIZZLE_B, GL_BLUE);
    MG_Impl::GLImpl::TexParameteri(GL_TEXTURE_2D_MULTISAMPLE_ARRAY, GL_TEXTURE_SWIZZLE_A, GL_ALPHA);
    MG_Impl::GLImpl::TexParameteri(GL_TEXTURE_2D_MULTISAMPLE_ARRAY, GL_TEXTURE_BASE_LEVEL, 0);
    MG_Impl::GLImpl::TexParameteri(GL_TEXTURE_2D_MULTISAMPLE_ARRAY, GL_TEXTURE_MAX_LEVEL, 1000);
    MG_Impl::GLImpl::TexImage3DMultisample(GL_TEXTURE_2D_MULTISAMPLE_ARRAY, 1, GL_RGBA8, 0, 0, 0, GL_TRUE);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR) << "GL_TEXTURE_2D_MULTISAMPLE_ARRAY reset failed";
}

// ---- GL CTS packed_pixels / texture_swizzle readback root-cause regressions --------------------

TEST_F(TextureTest, NormalizeLegacySizedFormatsMapToCanonicalShadowLayouts) {
    struct Case {
        GLenum requested;
        GLenum internalFormat;
        GLenum format;
        GLenum type;
    };
    const Case cases[] = {
        // Legacy <=8-bit-per-channel formats store as UNorm8 component arrays.
        {GL_R3_G3_B2, GL_RGB565, GL_RGB, GL_UNSIGNED_BYTE},
        {GL_RGB4, GL_RGB565, GL_RGB, GL_UNSIGNED_BYTE},
        {GL_RGB5, GL_RGB565, GL_RGB, GL_UNSIGNED_BYTE},
        {GL_RGBA2, GL_RGBA4, GL_RGBA, GL_UNSIGNED_BYTE},
        {GL_RGBA4, GL_RGBA4, GL_RGBA, GL_UNSIGNED_BYTE},
        {GL_RGB5_A1, GL_RGB5_A1, GL_RGBA, GL_UNSIGNED_BYTE},
        // 10/12-bit channels store as UNorm16 component arrays.
        {GL_RGB10, GL_RGB16, GL_RGB, GL_UNSIGNED_SHORT},
        {GL_RGB12, GL_RGB16, GL_RGB, GL_UNSIGNED_SHORT},
        {GL_RGBA12, GL_RGBA16, GL_RGBA, GL_UNSIGNED_SHORT},
        // RGB10_A2UI keeps its native packed layout (was previously unhandled -> broken uploads).
        {GL_RGB10_A2UI, GL_RGB10_A2UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT_2_10_10_10_REV},
    };
    for (const auto& testCase : cases) {
        GLenum internalFormat = 0;
        GLenum format = 0;
        GLenum type = 0;
        MG_Util::TextureFormatProcessor::NormalizePixelFormat(testCase.requested,
                                                              PixelFormatNormalizeOptionBit::None,
                                                              &internalFormat, &format, &type);
        EXPECT_EQ(internalFormat, testCase.internalFormat) << "requested 0x" << std::hex << testCase.requested;
        EXPECT_EQ(format, testCase.format) << "requested 0x" << std::hex << testCase.requested;
        EXPECT_EQ(type, testCase.type) << "requested 0x" << std::hex << testCase.requested;
    }
}

TEST_F(TextureTest, ConvertsUnsignedInt1010102PixelDataType) {
    // GL CTS packed_pixels uploads/reads GL_UNSIGNED_INT_10_10_10_2; the GL->MG mapping was missing,
    // rejecting every valid combination as GL_INVALID_ENUM.
    EXPECT_EQ(MG_Util::ConvertGLEnumToTexturePixelDataType(GL_UNSIGNED_INT_10_10_10_2),
              TexturePixelDataType::UnsignedInt1010102);
}

TEST_F(TextureTest, TexParameteriRejectsInvalidSwizzleValue) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);

    // GL CTS texture_swizzle.api_errors: values outside [RED, GREEN, BLUE, ALPHA, ZERO, ONE]
    // must raise GL_INVALID_ENUM through the single-value TexParameteri path.
    MG_Impl::GLImpl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RGB);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_ENUM);
    MG_Impl::GLImpl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, -1);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_ENUM);

    MG_Impl::GLImpl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_ALPHA);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, 0);
}

TEST_F(TextureTest, BoundTexImage2DEncodesPackedInternalShadowWords) {
    // RGB10_A2 / RGB9_E5 / R11F_G11F_B10F shadow bytes hold the ES upload word; uploads from
    // component client data must encode instead of raw-copying (GL CTS packed_pixels rgb10_a2,
    // rgb9_e5, r11f_g11f_b10f data comparisons).
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);

    const Uint8 rgba8[] = {255, 0, 0, 255};
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGB10_A2, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba8);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    {
        const auto* stored = GetBoundTexture2DLevelBytes(texture);
        Uint32 word = 0;
        std::memcpy(&word, stored, sizeof(word));
        EXPECT_EQ(word & 0x3FFu, 1023u);      // red = 1.0
        EXPECT_EQ((word >> 10) & 0x3FFu, 0u); // green = 0
        EXPECT_EQ((word >> 20) & 0x3FFu, 0u); // blue = 0
        EXPECT_EQ((word >> 30) & 0x3u, 3u);   // alpha = 1.0
    }

    const Float rgb[] = {1.0f, 0.5f, 0.25f};
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGB9_E5, 1, 1, 0, GL_RGB, GL_FLOAT, rgb);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    {
        const auto* stored = GetBoundTexture2DLevelBytes(texture);
        Uint32 word = 0;
        std::memcpy(&word, stored, sizeof(word));
        EXPECT_EQ(word, MG_Util::EncodeSharedExponentRGB9E5(rgb));
    }

    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_R11F_G11F_B10F, 1, 1, 0, GL_RGB, GL_FLOAT, rgb);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    {
        const auto* stored = GetBoundTexture2DLevelBytes(texture);
        Uint32 word = 0;
        std::memcpy(&word, stored, sizeof(word));
        const Uint32 expected = MG_Util::EncodeFloatToUnsignedF11(rgb[0]) |
                                (MG_Util::EncodeFloatToUnsignedF11(rgb[1]) << 11) |
                                (MG_Util::EncodeFloatToUnsignedF10(rgb[2]) << 22);
        EXPECT_EQ(word, expected);
    }

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, 0);
}

TEST_F(TextureTest, BoundTexImage2DDecodesPackedFloatSourceTypes) {
    // 5_9_9_9_REV / 10F_11F_11F_REV client data uploaded into a component internal format must be
    // decoded per texel (GL CTS packed_pixels uploads every RGB internal format with these types).
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);

    const Float rgb[] = {1.0f, 0.5f, 0.25f};
    const Uint32 word = MG_Util::EncodeSharedExponentRGB9E5(rgb);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 1, 1, 0, GL_RGB, GL_UNSIGNED_INT_5_9_9_9_REV, &word);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    const auto* stored = GetBoundTexture2DLevelBytes(texture);
    EXPECT_EQ(stored[0], 255); // 1.0
    EXPECT_EQ(stored[1], 128); // 0.5
    EXPECT_EQ(stored[2], 64);  // 0.25

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, 0);
}

TEST_F(TextureTest, UnpackSwapBytesSwapsComponentsNotWholePixels) {
    // GL_UNPACK_SWAP_BYTES on the identity-layout copy path used to reverse the whole pixel
    // (4 bytes for GL_RG16), garbling multi-component rows (GL CTS packed_pixels varied_rectangle
    // GL_UNPACK_SWAP_BYTES cases).
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);

    const Uint16 swapped[] = {0x3412, 0x7856}; // byte-swapped {0x1234, 0x5678}
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_SWAP_BYTES, GL_TRUE);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RG16, 1, 1, 0, GL_RG, GL_UNSIGNED_SHORT, swapped);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    const auto* stored = GetBoundTexture2DLevelBytes(texture);
    Uint16 red = 0;
    Uint16 green = 0;
    std::memcpy(&red, stored, sizeof(red));
    std::memcpy(&green, stored + 2, sizeof(green));
    EXPECT_EQ(red, 0x1234);
    EXPECT_EQ(green, 0x5678);

    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_SWAP_BYTES, GL_FALSE);
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, 0);
}

TEST_F(TextureTest, DecodeShadowDataToWideRGBACoversComponentAndPackedLayouts) {
    // GetTexImage of non-renderable formats reads the CPU shadow; the decode must cover both
    // component-array and packed internal layouts.
    Vector<Uint8> wide;
    Bool isInteger = false;
    Bool isSigned = false;

    const Uint8 r8[] = {128};
    ASSERT_TRUE(MG_Util::PixelStoreProcessor::DecodeShadowDataToWideRGBA(TextureInternalFormat::R8, r8, 1, wide,
                                                                         isInteger, isSigned));
    EXPECT_FALSE(isInteger);
    {
        Float rgba[4];
        std::memcpy(rgba, wide.data(), sizeof(rgba));
        EXPECT_NEAR(rgba[0], 128.0f / 255.0f, 1e-6f);
        EXPECT_EQ(rgba[1], 0.0f);
        EXPECT_EQ(rgba[2], 0.0f);
        EXPECT_EQ(rgba[3], 1.0f);
    }

    const Float rgb[] = {1.0f, 0.5f, 0.25f};
    const Uint32 e5Word = MG_Util::EncodeSharedExponentRGB9E5(rgb);
    ASSERT_TRUE(MG_Util::PixelStoreProcessor::DecodeShadowDataToWideRGBA(TextureInternalFormat::RGB9E5, &e5Word, 1,
                                                                         wide, isInteger, isSigned));
    EXPECT_FALSE(isInteger);
    {
        Float rgba[4];
        std::memcpy(rgba, wide.data(), sizeof(rgba));
        EXPECT_NEAR(rgba[0], 1.0f, 1.0f / 256.0f);
        EXPECT_NEAR(rgba[1], 0.5f, 1.0f / 256.0f);
        EXPECT_NEAR(rgba[2], 0.25f, 1.0f / 256.0f);
        EXPECT_EQ(rgba[3], 1.0f);
    }

    const Uint32 uiWord = 1023u | (511u << 10) | (255u << 20) | (2u << 30); // RGB10_A2UI
    ASSERT_TRUE(MG_Util::PixelStoreProcessor::DecodeShadowDataToWideRGBA(TextureInternalFormat::RGB10A2UI, &uiWord, 1,
                                                                         wide, isInteger, isSigned));
    EXPECT_TRUE(isInteger);
    EXPECT_FALSE(isSigned);
    {
        Uint32 rgba[4];
        std::memcpy(rgba, wide.data(), sizeof(rgba));
        EXPECT_EQ(rgba[0], 1023u);
        EXPECT_EQ(rgba[1], 511u);
        EXPECT_EQ(rgba[2], 255u);
        EXPECT_EQ(rgba[3], 2u);
    }
}
