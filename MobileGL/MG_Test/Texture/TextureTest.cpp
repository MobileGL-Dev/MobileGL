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
#include <MG_Backend/DirectGLES/Utils.h>
#include <MG_Impl/GLImpl/Buffer/GL_Buffer.h>
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
#include <MG_Util/Converters/MGToStr/TextureEnumConverter.h>
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

// GL 4.6 core 8.19: a compressed internal format is INVALID_OPERATION for both clear entry points.
// The generic GL_COMPRESSED_* enums are the half that needs its own tag - MobileGL answers them
// with uncompressed storage on purpose, so by the time the clear runs the level looks like any
// other RGBA8 image unless the REQUEST was recorded alongside it.
TEST_F(TextureTest, ClearTexImageRejectsCompressedTextures) {
    GLuint genericTexture = 0;
    MG_Impl::GLImpl::GenTextures(1, &genericTexture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, genericTexture);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::ClearTexImage(genericTexture, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    ExpectSingleGlError(GL_INVALID_OPERATION);
    MG_Impl::GLImpl::ClearTexSubImage(genericTexture, 0, 0, 0, 0, 4, 4, 1, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    ExpectSingleGlError(GL_INVALID_OPERATION);

    // A specific compressed internalformat is refused through the tag the level already carried...
    GLuint specificTexture = 0;
    MG_Impl::GLImpl::GenTextures(1, &specificTexture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, specificTexture);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RED_RGTC1, 8, 8, 0, GL_RED, GL_UNSIGNED_BYTE,
                                nullptr);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    MG_Impl::GLImpl::ClearTexImage(specificTexture, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    ExpectSingleGlError(GL_INVALID_OPERATION);

    // ...and respecifying the level with an uncompressed format makes it clearable again, because
    // AllocateStorage clears both tags.
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_R8, 8, 8, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    MG_Impl::GLImpl::ClearTexImage(specificTexture, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
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

// GL_STENCIL_INDEX is the unsized base format for stencil-only storage, and refusing it as an
// internal format killed the ARB_clear_texture stencil case in its own setup - before it could
// reach the calls it actually tests. The stencil-only transfer format stays paired with
// stencil-only storage in both directions, which is what keeps those clears erroring.
TEST_F(TextureTest, StencilIndexIsATextureInternalFormatPairedOnlyWithStencilStorage) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_STENCIL_INDEX, 4, 4, 0, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE,
                                nullptr);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    const auto textureObject = MG_State::pGLContext->GetTextureObject(texture);
    ASSERT_NE(textureObject, nullptr);
    EXPECT_EQ(textureObject->GetFormat(), TextureInternalFormat::StencilIndex8);

    // A colour transfer format against stencil storage is still INVALID_OPERATION, so the clear
    // the conformance case makes next fails the way it is supposed to.
    MG_Impl::GLImpl::ClearTexImage(texture, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    ExpectSingleGlError(GL_INVALID_OPERATION);

    // ...and the other direction: GL_STENCIL_INDEX against colour storage stays illegal.
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, nullptr);
    ExpectSingleGlError(GL_INVALID_OPERATION);
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

// GL 4.6 core 8.11.4 asks a readback for cube completeness and nothing else, so a mip chain whose
// levels BELOW the requested one were never defined is still readable at that level - which is
// exactly the shape ARB_clear_texture's conformance cases build (they define only the level they
// clear). The whole-chain completeness gate used to answer INVALID_OPERATION here.
TEST_F(TextureTest, GetTexImageReadsALevelWhoseLowerLevelsWereNeverDefined) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);

    const Uint8 pixels[] = {
        61, 62, 63, 64,
        71, 72, 73, 74,
    };
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 2, GL_RGBA8, 2, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    Uint8 output[sizeof(pixels)] = {};
    MG_Impl::GLImpl::GetTexImage(GL_TEXTURE_2D, 2, GL_RGBA, GL_UNSIGNED_BYTE, output);

    EXPECT_EQ(std::memcmp(output, pixels, sizeof(pixels)), 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// The other half of the same rule: loosening the chain-wide check must not let a level that holds
// no image at all through. Level 0 exists as a chain slot once level 2 is defined, but nothing ever
// gave it an image, so it stays INVALID_OPERATION - as does a level past the end of the chain and a
// texture that was never given any image whatsoever.
TEST_F(TextureTest, GetTexImageStillRejectsALevelThatHoldsNoImage) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);

    Uint8 output[4] = {};

    // No image at all yet: the chain carries no levels.
    MG_Impl::GLImpl::GetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, output);
    ExpectSingleGlError(GL_INVALID_OPERATION);

    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 2, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // Inside the chain, but never defined.
    MG_Impl::GLImpl::GetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, output);
    ExpectSingleGlError(GL_INVALID_OPERATION);

    // Past the end of the chain.
    MG_Impl::GLImpl::GetTexImage(GL_TEXTURE_2D, 3, GL_RGBA, GL_UNSIGNED_BYTE, output);
    ExpectSingleGlError(GL_INVALID_OPERATION);
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

// GL 4.6 core 8.19: for GL_TEXTURE_1D_ARRAY the `height` argument of glTexStorage2D is the LAYER
// COUNT, and an array texture's layer count "stays put all the way down the chain" (8.14.3) - only
// the image's own axes halve. Shrinking it made level i report height >> i layers, which is also
// what ComputeMipmapCompleteForFilter reads (it holds component 1 constant for this target), so
// every mipmapped 1D array texture judged itself incomplete.
TEST_F(TextureTest, TexStorage2DKeepsA1DArrayLayerCountConstantDownTheMipChain) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_1D_ARRAY, texture);

    constexpr GLsizei kLevels = 3;
    constexpr GLsizei kWidth = 4;
    constexpr GLsizei kLayers = 4;
    MG_Impl::GLImpl::TexStorage2D(GL_TEXTURE_1D_ARRAY, kLevels, GL_RGBA8, kWidth, kLayers);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    const auto textureObject = MG_State::pGLContext->GetTextureObject(texture);
    auto* mipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(textureObject.get());
    ASSERT_NE(mipmapObject, nullptr);
    ASSERT_EQ(mipmapObject->GetMipmapLevelCount(), static_cast<Uint>(kLevels));

    for (GLsizei level = 0; level < kLevels; ++level) {
        const IntVec3 size =
            mipmapObject->GetMipmapTexelSize(TextureUploadTarget::Texture1DArray, static_cast<Uint>(level));
        EXPECT_EQ(size.x(), std::max<GLsizei>(1, kWidth >> level)) << "level " << level << " width";
        EXPECT_EQ(size.y(), kLayers) << "level " << level << " must keep every layer";
    }

    // The completeness walk is the reason this matters beyond the reported extent.
    EXPECT_TRUE(textureObject->IsComplete());
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

// Resolving to uncompressed storage is a storage decision, not a licence to answer the level
// queries as if the application had asked for an uncompressed format. GL 4.6 core 8.5 lets the
// implementation choose for the GENERIC formats (GL_COMPRESSED_RED and friends), but a SPECIFIC
// one commits the level: GL_TEXTURE_COMPRESSED is true, GL_TEXTURE_INTERNAL_FORMAT is the token
// that was passed, and GL_TEXTURE_COMPRESSED_IMAGE_SIZE answers instead of erroring - which is
// exactly the three-query sequence KHR-GL44.buffer_storage.map_persistent_texture opens with to
// size the image it then uploads through glCompressedTexSubImage2D.
TEST_F(TextureTest, ASpecificCompressedInternalFormatTagsTheLevelCompressed) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RED_RGTC1, 8, 8, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    GLint compressed = GL_FALSE;
    MG_Impl::GLImpl::GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_COMPRESSED, &compressed);
    EXPECT_EQ(compressed, GL_TRUE);

    GLint internalFormat = 0;
    MG_Impl::GLImpl::GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &internalFormat);
    EXPECT_EQ(internalFormat, static_cast<GLint>(GL_COMPRESSED_RED_RGTC1));

    // 8x8 in 4x4 blocks of 8 bytes each.
    GLint imageSize = 0;
    MG_Impl::GLImpl::GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_COMPRESSED_IMAGE_SIZE, &imageSize);
    EXPECT_EQ(imageSize, 32);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // The texel shadow behind the tag still carries the uncompressed storage the format resolves
    // to - which is what lets the level sample, and what every size computation downstream
    // divides by.
    const auto textureObject = MG_State::pGLContext->GetTextureObject(texture);
    ASSERT_NE(textureObject, nullptr);
    EXPECT_EQ(textureObject->GetFormat(), TextureInternalFormat::R8);
}

// The negative control for the case above, and the reason it cannot simply tag every
// GL_COMPRESSED_* token: for a generic format the implementation's choice IS the answer, and
// MobileGL chooses uncompressed - so the level is not compressed and the size query is the
// INVALID_OPERATION GL 4.6 core 8.11 prescribes for an uncompressed image.
TEST_F(TextureTest, AGenericCompressedInternalFormatLeavesTheLevelUncompressed) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RED, 8, 8, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    GLint compressed = GL_TRUE;
    MG_Impl::GLImpl::GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_COMPRESSED, &compressed);
    EXPECT_EQ(compressed, GL_FALSE);

    GLint internalFormat = 0;
    MG_Impl::GLImpl::GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &internalFormat);
    EXPECT_EQ(internalFormat, static_cast<GLint>(GL_R8));
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    GLint imageSize = 0;
    MG_Impl::GLImpl::GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_COMPRESSED_IMAGE_SIZE, &imageSize);
    ExpectSingleGlError(GL_INVALID_OPERATION);
}

// A plain glTexImage2D over a level that was tagged compressed has to un-tag it, the same way it
// does for a level a glCompressedTexImage2D shadowed - otherwise the size query would keep
// answering for an image that no longer exists.
TEST_F(TextureTest, AnUncompressedRespecificationClearsTheCompressedTag) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RED_RGTC1, 8, 8, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_R8, 8, 8, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    GLint compressed = GL_TRUE;
    MG_Impl::GLImpl::GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_COMPRESSED, &compressed);
    EXPECT_EQ(compressed, GL_FALSE);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// The same rule for the 3D entry points, which never recorded the tag at all. Besides the two
// level queries this decides the level's texel BLOCK SIZE, which glCopyImageSubData compares
// against the other endpoint's - an untagged GL_COMPRESSED_RG_RGTC2 array level measured as the
// RG8 storage it resolves to, 2 bytes instead of 16.
TEST_F(TextureTest, TexImage3DAndTexStorage3DTagASpecificCompressedInternalFormat) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D_ARRAY, texture);
    MG_Impl::GLImpl::TexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_COMPRESSED_RG_RGTC2, 8, 8, 2, 0, GL_RG,
                                GL_UNSIGNED_BYTE, nullptr);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    GLint compressed = GL_FALSE;
    MG_Impl::GLImpl::GetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_COMPRESSED, &compressed);
    EXPECT_EQ(compressed, GL_TRUE);

    GLint internalFormat = 0;
    MG_Impl::GLImpl::GetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_INTERNAL_FORMAT, &internalFormat);
    EXPECT_EQ(internalFormat, static_cast<GLint>(GL_COMPRESSED_RG_RGTC2));

    // 8x8 in 4x4 blocks of 16 bytes each is 64 bytes a layer, and both layers count.
    GLint imageSize = 0;
    MG_Impl::GLImpl::GetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_COMPRESSED_IMAGE_SIZE, &imageSize);
    EXPECT_EQ(imageSize, 128);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // The texel shadow behind the tag keeps the uncompressed storage the format resolves to.
    const auto textureObject = MG_State::pGLContext->GetTextureObject(texture);
    ASSERT_NE(textureObject, nullptr);
    EXPECT_EQ(textureObject->GetFormat(), TextureInternalFormat::RG8);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D_ARRAY, 0);

    // glTexStorage3D has the same gap and the same fix; immutable storage plus
    // glCompressedTexSubImage3D is the modern way to upload a compressed array texture.
    GLuint storageTexture = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D_ARRAY, 1, &storageTexture);
    MG_Impl::GLImpl::TextureStorage3D(storageTexture, 1, GL_COMPRESSED_RG_RGTC2, 8, 8, 2);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D_ARRAY, storageTexture);

    compressed = GL_FALSE;
    MG_Impl::GLImpl::GetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_COMPRESSED, &compressed);
    EXPECT_EQ(compressed, GL_TRUE);
    imageSize = 0;
    MG_Impl::GLImpl::GetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_COMPRESSED_IMAGE_SIZE, &imageSize);
    EXPECT_EQ(imageSize, 128);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D_ARRAY, 0);
}

namespace {
    // A 16x16 RGBA8 texture with exactly `levelCount` levels, defined the way
    // KHR-GL43.copy_image.non_existent_mipmap defines its textures - glTexImage2D per
    // level, NOT glTexStorage2D, because an immutable allocation defines the whole chain
    // up front and so cannot express "level 1 does not exist".
    GLuint MakeCopyImageTexture(int levelCount) {
        GLuint texture = 0;
        MG_Impl::GLImpl::GenTextures(1, &texture);
        MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
        for (int level = 0; level < levelCount; ++level) {
            const GLsizei extent = 16 >> level;
            MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, level, GL_RGBA8, extent, extent, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                                        nullptr);
        }
        return texture;
    }
} // namespace

// KHR-GL43.copy_image.non_existent_mipmap. Level 1 of a texture that has only level 0 is
// not a level: GL 4.6 core 18.3.2 asks for GL_INVALID_VALUE. Until this check existed the
// level travelled all the way into the backends, and DirectVulkan built a VkImageCopy
// naming mip 1 of a VkImage created with one mip - which Adreno answered with a SIGSEGV
// inside vkCmdCopyImage, killing the glcts process in the middle of a negative test.
TEST_F(TextureTest, CopyImageSubDataRejectsALevelTheTextureDoesNotHave) {
    const GLuint src = MakeCopyImageTexture(1);
    const GLuint dst = MakeCopyImageTexture(1);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::CopyImageSubData(src, GL_TEXTURE_2D, 1, 0, 0, 0, dst, GL_TEXTURE_2D, 0, 0, 0, 0, 1, 1, 1);
    ExpectSingleGlError(GL_INVALID_VALUE);

    MG_Impl::GLImpl::CopyImageSubData(src, GL_TEXTURE_2D, 0, 0, 0, 0, dst, GL_TEXTURE_2D, 1, 0, 0, 0, 1, 1, 1);
    ExpectSingleGlError(GL_INVALID_VALUE);

    MG_Impl::GLImpl::CopyImageSubData(src, GL_TEXTURE_2D, 1, 0, 0, 0, dst, GL_TEXTURE_2D, 1, 0, 0, 0, 1, 1, 1);
    ExpectSingleGlError(GL_INVALID_VALUE);
}

// The negative control, and the reason the pair below asks for a zero-sized copy: a
// validator that answered GL_INVALID_VALUE to every non-zero level would satisfy the test
// above. The two calls here are IDENTICAL except for how many levels the textures have,
// and a zero extent makes the validator decline the copy without an error just after the
// level check - so the level count is the only thing either assertion can be reading, and
// no backend (there is none in this binary) is ever reached.
TEST_F(TextureTest, CopyImageSubDataAcceptsALevelTheTextureDoesHave) {
    const GLuint oneLevelSrc = MakeCopyImageTexture(1);
    const GLuint oneLevelDst = MakeCopyImageTexture(1);
    const GLuint twoLevelSrc = MakeCopyImageTexture(2);
    const GLuint twoLevelDst = MakeCopyImageTexture(2);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::CopyImageSubData(oneLevelSrc, GL_TEXTURE_2D, 1, 0, 0, 0, oneLevelDst, GL_TEXTURE_2D, 1, 0, 0, 0,
                                      0, 0, 0);
    ExpectSingleGlError(GL_INVALID_VALUE);

    MG_Impl::GLImpl::CopyImageSubData(twoLevelSrc, GL_TEXTURE_2D, 1, 0, 0, 0, twoLevelDst, GL_TEXTURE_2D, 1, 0, 0, 0,
                                      0, 0, 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR) << "level 1 of a two-level texture is a level";

    // And the boundary from the other side: two levels means 0 and 1, not 2.
    MG_Impl::GLImpl::CopyImageSubData(twoLevelSrc, GL_TEXTURE_2D, 2, 0, 0, 0, twoLevelDst, GL_TEXTURE_2D, 0, 0, 0, 0,
                                      0, 0, 0);
    ExpectSingleGlError(GL_INVALID_VALUE);
}

// A texture that has never been given an image is a different fault from a level out of
// range, and the spec spells it differently: an incomplete object named by a copy is
// GL_INVALID_OPERATION. Worth pinning because the natural implementation of the check
// above - level >= levelCount - reports INVALID_VALUE for level 0 of a texture whose level
// count is zero, which is the wrong answer to the wrong question.
//
// BOTH textures are imageless on purpose, and that is the whole point rather than symmetry
// for its own sake. With one imageless and one RGBA8 texture the format comparison further
// down already rejected the call, so the case proved nothing about this check. With both
// imageless the formats are Unknown == Unknown, they MATCH, and every validator downstream
// waves the call through - which is how the second crash in this entry point was found: the
// call reached DirectVulkan, SyncTextureAndGetDescriptor returned nothing for a texture with
// no image, and the release build (where the guarding MOBILEGL_ASSERT expands to nothing)
// dereferenced it. Reproduced deterministically on lavapipe by
// KHR-GL43.copy_image.functional_src_target_texture_2d_array_..._dst_format_rgb9_e5.
TEST_F(TextureTest, CopyImageSubDataRejectsTwoTexturesWithNoImageAtAll) {
    GLuint firstEmpty = 0;
    GLuint secondEmpty = 0;
    MG_Impl::GLImpl::GenTextures(1, &firstEmpty);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, firstEmpty);
    MG_Impl::GLImpl::GenTextures(1, &secondEmpty);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, secondEmpty);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::CopyImageSubData(firstEmpty, GL_TEXTURE_2D, 0, 0, 0, 0, secondEmpty, GL_TEXTURE_2D, 0, 0, 0, 0,
                                      1, 1, 1);
    ExpectSingleGlError(GL_INVALID_OPERATION);
}

// GL_DEPTH_STENCIL_TEXTURE_MODE used to be a pure frontend shadow: stored, answered by
// glGetTexParameter, and never shown to a backend. Sampling therefore always read the depth
// aspect however the mode was set, which is the whole of
// KHR-GL3x.packed_depth_stencil.stencil_texturing. Both backends pick the aspect up through the
// texture-params version - DirectGLES re-emits glTexParameteri when it moves, DirectVulkan
// rebuilds the sampled image view - so the version bump is the load-bearing part, and a
// no-op write must not spend one (every bump costs DirectVulkan a view recreation).
TEST_F(TextureTest, DepthStencilTextureModeIsBackendVisibleThroughTheParamsVersion) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
    MG_Impl::GLImpl::TexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH24_STENCIL8, 8, 8);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    const auto textureObject = MG_State::pGLContext->GetTextureObject(texture);
    ASSERT_NE(textureObject, nullptr);
    EXPECT_EQ(textureObject->GetDepthStencilTextureMode(), static_cast<GLenum>(GL_DEPTH_COMPONENT));

    const Uint16 initialVersion = textureObject->GetTextureParamsVersion();
    MG_Impl::GLImpl::TexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_STENCIL_INDEX);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_EQ(textureObject->GetDepthStencilTextureMode(), static_cast<GLenum>(GL_STENCIL_INDEX));
    EXPECT_NE(textureObject->GetTextureParamsVersion(), initialVersion);

    // Re-writing the value already in force is not a change and must not invalidate anything.
    const Uint16 settledVersion = textureObject->GetTextureParamsVersion();
    MG_Impl::GLImpl::TexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_STENCIL_INDEX);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_EQ(textureObject->GetTextureParamsVersion(), settledVersion);

    // ...and going back to the depth aspect is a change again.
    MG_Impl::GLImpl::TexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_DEPTH_COMPONENT);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_EQ(textureObject->GetDepthStencilTextureMode(), static_cast<GLenum>(GL_DEPTH_COMPONENT));
    EXPECT_NE(textureObject->GetTextureParamsVersion(), settledVersion);
}

namespace {
    // 8x8 RGTC1: 2x2 blocks of 8 bytes, so the stored image is 32 bytes and one block row is 16.
    constexpr GLsizei kRgtc1Size8x8 = 32;

    GLuint MakeCompressedRgtc1Texture8x8() {
        GLuint texture = 0;
        MG_Impl::GLImpl::GenTextures(1, &texture);
        MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
        MG_Impl::GLImpl::CompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RED_RGTC1, 8, 8, 0, kRgtc1Size8x8,
                                              nullptr);
        return texture;
    }
} // namespace

// glCompressedTexSubImage2D was a stub that answered GL_INVALID_ENUM to every call. It replaces a
// block-aligned rectangle of the stored image, and the arithmetic that places the incoming blocks
// is what the partial write below pins: a full-width write would pass with the rows concatenated
// in either order.
TEST_F(TextureTest, CompressedTexSubImage2DReplacesTheStoredBlocks) {
    const GLuint texture = MakeCompressedRgtc1Texture8x8();
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    Uint8 whole[kRgtc1Size8x8];
    for (Int i = 0; i < kRgtc1Size8x8; ++i) whole[i] = static_cast<Uint8>(i + 1);
    MG_Impl::GLImpl::CompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 8, 8, GL_COMPRESSED_RED_RGTC1, kRgtc1Size8x8,
                                             whole);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    Uint8 stored[kRgtc1Size8x8] = {};
    MG_Impl::GLImpl::GetCompressedTexImage(GL_TEXTURE_2D, 0, stored);
    EXPECT_EQ(std::memcmp(stored, whole, sizeof(whole)), 0);

    // The right-hand block column only: one block wide, two block rows high. Its two blocks land
    // at byte 8 and byte 24, not at bytes 0 and 8.
    const Uint8 column[16] = {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
                              0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7};
    MG_Impl::GLImpl::CompressedTexSubImage2D(GL_TEXTURE_2D, 0, 4, 0, 4, 8, GL_COMPRESSED_RED_RGTC1,
                                             static_cast<GLsizei>(sizeof(column)), column);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    Uint8 expected[kRgtc1Size8x8];
    std::memcpy(expected, whole, sizeof(expected));
    std::memcpy(expected + 8, column, 8);
    std::memcpy(expected + 24, column + 8, 8);

    std::memset(stored, 0, sizeof(stored));
    MG_Impl::GLImpl::GetCompressedTexImage(GL_TEXTURE_2D, 0, stored);
    EXPECT_EQ(std::memcmp(stored, expected, sizeof(expected)), 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// The Y axis of the placement, which the whole-image and single-column cases above cannot see: an
// implementation that dropped the first-block-row term, or that divided yoffset by the block WIDTH,
// passes every one of them. The region here starts at block row 1, so its two blocks belong at
// bytes 16 and 24 and nowhere else.
TEST_F(TextureTest, CompressedTexSubImage2DPlacesTheFirstBlockRow) {
    const GLuint texture = MakeCompressedRgtc1Texture8x8();
    Uint8 whole[kRgtc1Size8x8];
    for (Int i = 0; i < kRgtc1Size8x8; ++i) whole[i] = static_cast<Uint8>(i + 1);
    MG_Impl::GLImpl::CompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 8, 8, GL_COMPRESSED_RED_RGTC1, kRgtc1Size8x8,
                                             whole);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // The bottom block row only: 8 texels wide, 4 high, starting at y = 4.
    const Uint8 bottom[16] = {0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7,
                              0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7};
    MG_Impl::GLImpl::CompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 4, 8, 4, GL_COMPRESSED_RED_RGTC1,
                                             static_cast<GLsizei>(sizeof(bottom)), bottom);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    Uint8 expected[kRgtc1Size8x8];
    std::memcpy(expected, whole, sizeof(expected));
    std::memcpy(expected + 16, bottom, sizeof(bottom));

    Uint8 stored[kRgtc1Size8x8] = {};
    MG_Impl::GLImpl::GetCompressedTexImage(GL_TEXTURE_2D, 0, stored);
    EXPECT_EQ(std::memcmp(stored, expected, sizeof(expected)), 0);

    // And one block in the far corner, which needs both terms at once.
    const Uint8 corner[8] = {0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7};
    MG_Impl::GLImpl::CompressedTexSubImage2D(GL_TEXTURE_2D, 0, 4, 4, 4, 4, GL_COMPRESSED_RED_RGTC1,
                                             static_cast<GLsizei>(sizeof(corner)), corner);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    std::memcpy(expected + 24, corner, sizeof(corner));
    std::memset(stored, 0, sizeof(stored));
    MG_Impl::GLImpl::GetCompressedTexImage(GL_TEXTURE_2D, 0, stored);
    EXPECT_EQ(std::memcmp(stored, expected, sizeof(expected)), 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    (void)texture;
}

// A level whose size is neither square nor a multiple of the block size, at a level above the
// base, in a format with SIXTEEN bytes per block. Between them these pin the row stride (which a
// square level cannot distinguish from the column count), the rounding-up of a partial edge block,
// the run-to-the-edge exemption from the whole-blocks rule, and the block size actually coming from
// the format rather than from a constant.
TEST_F(TextureTest, CompressedTexSubImage2DHandlesPartialBlocksAndAMipLevel) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
    // 6x10 BPTC: 2 block columns x 3 block rows of 16 bytes = 96, one block row = 32.
    constexpr GLsizei kBptcSize6x10 = 96;
    MG_Impl::GLImpl::CompressedTexImage2D(GL_TEXTURE_2D, 1, GL_COMPRESSED_RGBA_BPTC_UNORM, 6, 10, 0, kBptcSize6x10,
                                          nullptr);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    GLint imageSize = 0;
    MG_Impl::GLImpl::GetTexLevelParameteriv(GL_TEXTURE_2D, 1, GL_TEXTURE_COMPRESSED_IMAGE_SIZE, &imageSize);
    EXPECT_EQ(imageSize, kBptcSize6x10);

    Uint8 whole[kBptcSize6x10];
    for (Int i = 0; i < kBptcSize6x10; ++i) whole[i] = static_cast<Uint8>(i + 1);
    MG_Impl::GLImpl::CompressedTexSubImage2D(GL_TEXTURE_2D, 1, 0, 0, 6, 10, GL_COMPRESSED_RGBA_BPTC_UNORM,
                                             kBptcSize6x10, whole);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // The right-hand column (2 texels wide - a partial block that runs to the edge) of the middle
    // block row: one block, at byte 32 + 16.
    Uint8 patch[16];
    for (Int i = 0; i < 16; ++i) patch[i] = static_cast<Uint8>(0xF0 + i);
    MG_Impl::GLImpl::CompressedTexSubImage2D(GL_TEXTURE_2D, 1, 4, 4, 2, 4, GL_COMPRESSED_RGBA_BPTC_UNORM,
                                             static_cast<GLsizei>(sizeof(patch)), patch);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    Uint8 expected[kBptcSize6x10];
    std::memcpy(expected, whole, sizeof(expected));
    std::memcpy(expected + 48, patch, sizeof(patch));

    Uint8 stored[kBptcSize6x10] = {};
    MG_Impl::GLImpl::GetCompressedTexImage(GL_TEXTURE_2D, 1, stored);
    EXPECT_EQ(std::memcmp(stored, expected, sizeof(expected)), 0);

    // The partial edge block is only exempt from the whole-blocks rule AT the edge: the same
    // 2-texel width one block to the left is not.
    MG_Impl::GLImpl::CompressedTexSubImage2D(GL_TEXTURE_2D, 1, 0, 4, 2, 4, GL_COMPRESSED_RGBA_BPTC_UNORM,
                                             static_cast<GLsizei>(sizeof(patch)), patch);
    ExpectSingleGlError(GL_INVALID_OPERATION);
}

// The same call sourcing its blocks from a buffer bound to GL_PIXEL_UNPACK_BUFFER, where `data` is
// an offset into that buffer rather than a client pointer - which is the form
// KHR-GL44.buffer_storage.map_persistent_texture uses for every one of its operations.
TEST_F(TextureTest, CompressedTexSubImage2DUnpacksFromAPixelUnpackBuffer) {
    Uint8 source[256];
    for (Int i = 0; i < 256; ++i) source[i] = static_cast<Uint8>(i);
    GLuint buffer = 0;
    MG_Impl::GLImpl::GenBuffers(1, &buffer);
    MG_Impl::GLImpl::BindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer);
    MG_Impl::GLImpl::BufferData(GL_PIXEL_UNPACK_BUFFER, sizeof(source), source, GL_STATIC_DRAW);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    const GLuint texture = MakeCompressedRgtc1Texture8x8();
    MG_Impl::GLImpl::CompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 8, 8, GL_COMPRESSED_RED_RGTC1, kRgtc1Size8x8,
                                             reinterpret_cast<const void*>(static_cast<SizeT>(64)));
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    Uint8 stored[kRgtc1Size8x8] = {};
    MG_Impl::GLImpl::GetCompressedTexImage(GL_TEXTURE_2D, 0, stored);
    EXPECT_EQ(std::memcmp(stored, source + 64, sizeof(stored)), 0);

    // Reading past the end of the buffer is the unpack-buffer error, not a read out of bounds.
    MG_Impl::GLImpl::CompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 8, 8, GL_COMPRESSED_RED_RGTC1, kRgtc1Size8x8,
                                             reinterpret_cast<const void*>(static_cast<SizeT>(sizeof(source) - 8)));
    ExpectSingleGlError(GL_INVALID_OPERATION);

    MG_Impl::GLImpl::BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    (void)texture;
}

// ARB_buffer_storage's whole point: a PERSISTENTLY mapped buffer stays usable while the map is
// live, including as the source of a texture upload - which is what
// KHR-GL44.buffer_storage.map_persistent_texture checks. An ordinary map still disqualifies it.
// Both compressed entry points share one validator, so both are checked here.
TEST_F(TextureTest, CompressedUploadsAcceptAPersistentlyMappedUnpackBuffer) {
    Uint8 source[256];
    for (Int i = 0; i < 256; ++i) source[i] = static_cast<Uint8>(255 - i);
    GLuint buffer = 0;
    MG_Impl::GLImpl::GenBuffers(1, &buffer);
    MG_Impl::GLImpl::BindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer);
    MG_Impl::GLImpl::BufferStorage(GL_PIXEL_UNPACK_BUFFER, sizeof(source), source,
                                   GL_MAP_PERSISTENT_BIT | GL_MAP_READ_BIT | GL_MAP_WRITE_BIT);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    void* mapped = MG_Impl::GLImpl::MapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, sizeof(source),
                                                   GL_MAP_PERSISTENT_BIT | GL_MAP_READ_BIT | GL_MAP_WRITE_BIT);
    ASSERT_NE(mapped, nullptr);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
    // The image call takes offset 0 and the sub-image call offset 128, so the readback can only
    // match if the SUB-IMAGE call ran: were it refused (or a no-op), the level would still hold
    // the image call's bytes.
    MG_Impl::GLImpl::CompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RED_RGTC1, 8, 8, 0, kRgtc1Size8x8,
                                          reinterpret_cast<const void*>(static_cast<SizeT>(0)));
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR) << "glCompressedTexImage2D over a persistent map";
    MG_Impl::GLImpl::CompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 8, 8, GL_COMPRESSED_RED_RGTC1, kRgtc1Size8x8,
                                             reinterpret_cast<const void*>(static_cast<SizeT>(128)));
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR) << "glCompressedTexSubImage2D over a persistent map";

    Uint8 stored[kRgtc1Size8x8] = {};
    MG_Impl::GLImpl::GetCompressedTexImage(GL_TEXTURE_2D, 0, stored);
    EXPECT_EQ(std::memcmp(stored, source + 128, sizeof(stored)), 0);

    MG_Impl::GLImpl::UnmapBuffer(GL_PIXEL_UNPACK_BUFFER);

    // The negative control: an ORDINARY map is still an error, so the check above is not just
    // "the mapped test was dropped".
    GLuint plainBuffer = 0;
    MG_Impl::GLImpl::GenBuffers(1, &plainBuffer);
    MG_Impl::GLImpl::BindBuffer(GL_PIXEL_UNPACK_BUFFER, plainBuffer);
    MG_Impl::GLImpl::BufferData(GL_PIXEL_UNPACK_BUFFER, sizeof(source), source, GL_STATIC_DRAW);
    ASSERT_NE(MG_Impl::GLImpl::MapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_READ_ONLY), nullptr);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    MG_Impl::GLImpl::CompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 8, 8, GL_COMPRESSED_RED_RGTC1, kRgtc1Size8x8,
                                             reinterpret_cast<const void*>(static_cast<SizeT>(0)));
    ExpectSingleGlError(GL_INVALID_OPERATION);
    MG_Impl::GLImpl::UnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
    MG_Impl::GLImpl::BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
}

// glCompressedTextureSubImage2D was an exported no-op that raised no error at all, so an
// application could not tell the write had not happened. It must reach the NAMED texture and leave
// the binding it borrowed exactly as it found it.
TEST_F(TextureTest, CompressedTextureSubImage2DModifiesTheNamedTextureOnly) {
    const GLuint bound = MakeCompressedRgtc1Texture8x8();
    Uint8 boundImage[kRgtc1Size8x8];
    for (Int i = 0; i < kRgtc1Size8x8; ++i) boundImage[i] = 0x11;
    MG_Impl::GLImpl::CompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 8, 8, GL_COMPRESSED_RED_RGTC1, kRgtc1Size8x8,
                                             boundImage);

    const GLuint named = MakeCompressedRgtc1Texture8x8();
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, bound); // `named` is NOT the bound texture
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    Uint8 namedImage[kRgtc1Size8x8];
    for (Int i = 0; i < kRgtc1Size8x8; ++i) namedImage[i] = 0x22;
    MG_Impl::GLImpl::CompressedTextureSubImage2D(named, 0, 0, 0, 8, 8, GL_COMPRESSED_RED_RGTC1, kRgtc1Size8x8,
                                                 namedImage);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // The borrowed binding is back, and it kept its own image.
    Uint8 stored[kRgtc1Size8x8] = {};
    MG_Impl::GLImpl::GetCompressedTexImage(GL_TEXTURE_2D, 0, stored);
    EXPECT_EQ(std::memcmp(stored, boundImage, sizeof(stored)), 0);

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, named);
    std::memset(stored, 0, sizeof(stored));
    MG_Impl::GLImpl::GetCompressedTexImage(GL_TEXTURE_2D, 0, stored);
    EXPECT_EQ(std::memcmp(stored, namedImage, sizeof(stored)), 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

namespace {
    // 8x8x8 RGTC1: 2x2 blocks of 8 bytes per slice, so a slice is 32 bytes and the stack is 256.
    constexpr GLsizei kRgtc1Size8x8x8 = 256;
    constexpr GLsizei kRgtc1Slice8x8 = 32;

    GLuint MakeCompressedRgtc1Texture3D() {
        GLuint texture = 0;
        MG_Impl::GLImpl::GenTextures(1, &texture);
        MG_Impl::GLImpl::BindTexture(GL_TEXTURE_3D, texture);
        MG_Impl::GLImpl::CompressedTexImage3D(GL_TEXTURE_3D, 0, GL_COMPRESSED_RED_RGTC1, 8, 8, 8, 0, kRgtc1Size8x8x8,
                                              nullptr);
        return texture;
    }
} // namespace

// glCompressedTexImage3D used to answer GL_INVALID_ENUM to every call, which is what threw
// KHR-GL45.direct_state_access.textures_compressed_subimage out with an InternalError: the CTS
// asserts no error on it. A 3D compressed image is a stack of per-slice block grids, and the whole
// stack has to come back byte for byte.
TEST_F(TextureTest, CompressedTexImage3DShadowsTheWholeStackForReadback) {
    Uint8 whole[kRgtc1Size8x8x8];
    for (Int i = 0; i < kRgtc1Size8x8x8; ++i) whole[i] = static_cast<Uint8>(i);

    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_3D, texture);
    MG_Impl::GLImpl::CompressedTexImage3D(GL_TEXTURE_3D, 0, GL_COMPRESSED_RED_RGTC1, 8, 8, 8, 0, kRgtc1Size8x8x8,
                                          whole);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    Uint8 stored[kRgtc1Size8x8x8] = {};
    MG_Impl::GLImpl::GetCompressedTexImage(GL_TEXTURE_3D, 0, stored);
    EXPECT_EQ(std::memcmp(stored, whole, sizeof(whole)), 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // An imageSize that is not the one the format and the three dimensions imply - the depth axis
    // is the term a 2D-shaped size calculation would drop.
    MG_Impl::GLImpl::CompressedTexImage3D(GL_TEXTURE_3D, 0, GL_COMPRESSED_RED_RGTC1, 8, 8, 8, 0, kRgtc1Slice8x8,
                                          whole);
    ExpectSingleGlError(GL_INVALID_VALUE);
}

// Where the incoming blocks land. The box below is one block wide, one block high and two slices
// deep, starting at block (1,1) of slice 3: an implementation that dropped the slice stride, the
// block-row term or the block-column term puts them somewhere else, and a full-image write would
// hide all three.
TEST_F(TextureTest, CompressedTexSubImage3DPlacesBlocksSliceBySlice) {
    const GLuint texture = MakeCompressedRgtc1Texture3D();
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    Uint8 zeros[kRgtc1Size8x8x8] = {};
    MG_Impl::GLImpl::CompressedTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, 8, 8, 8, GL_COMPRESSED_RED_RGTC1,
                                             kRgtc1Size8x8x8, zeros);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    const Uint8 box[16] = {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
                           0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7};
    MG_Impl::GLImpl::CompressedTexSubImage3D(GL_TEXTURE_3D, 0, 4, 4, 3, 4, 4, 2, GL_COMPRESSED_RED_RGTC1,
                                             static_cast<GLsizei>(sizeof(box)), box);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    Uint8 expected[kRgtc1Size8x8x8] = {};
    // slice 3, block row 1, block column 1 -> 3*32 + 1*16 + 1*8, and the same place one slice on.
    std::memcpy(expected + 3 * kRgtc1Slice8x8 + 16 + 8, box, 8);
    std::memcpy(expected + 4 * kRgtc1Slice8x8 + 16 + 8, box + 8, 8);

    Uint8 stored[kRgtc1Size8x8x8] = {};
    MG_Impl::GLImpl::GetCompressedTexImage(GL_TEXTURE_3D, 0, stored);
    EXPECT_EQ(std::memcmp(stored, expected, sizeof(expected)), 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// glCompressedTextureSubImage3D was an exported no-op that raised no error at all. It must reach the
// NAMED texture and leave the binding it borrowed exactly as it found it.
TEST_F(TextureTest, CompressedTextureSubImage3DModifiesTheNamedTextureOnly) {
    const GLuint bound = MakeCompressedRgtc1Texture3D();
    Uint8 boundImage[kRgtc1Size8x8x8];
    std::memset(boundImage, 0x11, sizeof(boundImage));
    MG_Impl::GLImpl::CompressedTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, 8, 8, 8, GL_COMPRESSED_RED_RGTC1,
                                             kRgtc1Size8x8x8, boundImage);

    const GLuint named = MakeCompressedRgtc1Texture3D();
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_3D, bound); // `named` is NOT the bound texture
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    Uint8 namedImage[kRgtc1Size8x8x8];
    std::memset(namedImage, 0x22, sizeof(namedImage));
    MG_Impl::GLImpl::CompressedTextureSubImage3D(named, 0, 0, 0, 0, 8, 8, 8, GL_COMPRESSED_RED_RGTC1,
                                                 kRgtc1Size8x8x8, namedImage);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    Uint8 stored[kRgtc1Size8x8x8] = {};
    MG_Impl::GLImpl::GetCompressedTexImage(GL_TEXTURE_3D, 0, stored);
    EXPECT_EQ(std::memcmp(stored, boundImage, sizeof(stored)), 0);

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_3D, named);
    std::memset(stored, 0, sizeof(stored));
    MG_Impl::GLImpl::GetCompressedTexImage(GL_TEXTURE_3D, 0, stored);
    EXPECT_EQ(std::memcmp(stored, namedImage, sizeof(stored)), 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

TEST_F(TextureTest, CompressedTexSubImage3DRejectsTheRegionsGLForbids) {
    const GLuint texture = MakeCompressedRgtc1Texture3D();
    Uint8 blocks[kRgtc1Size8x8x8] = {};
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // A format that is not the one the image is stored in.
    MG_Impl::GLImpl::CompressedTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, 8, 8, 8, GL_COMPRESSED_RG_RGTC2, 512, blocks);
    ExpectSingleGlError(GL_INVALID_OPERATION);

    // A start that is not on a block boundary.
    MG_Impl::GLImpl::CompressedTexSubImage3D(GL_TEXTURE_3D, 0, 2, 0, 0, 4, 8, 8, GL_COMPRESSED_RED_RGTC1, 128, blocks);
    ExpectSingleGlError(GL_INVALID_OPERATION);

    // A box that runs past the last slice - the depth bound a 2D-shaped range check never applies.
    MG_Impl::GLImpl::CompressedTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 6, 8, 8, 4, GL_COMPRESSED_RED_RGTC1, 128, blocks);
    ExpectSingleGlError(GL_INVALID_VALUE);

    (void)texture;
}

// The DSA name rule the CTS's textures_creation pair does not reach for these two entry points: a
// name handed out by glGenTextures has no object until it is first bound, so a by-name call on it is
// INVALID_OPERATION - and, unlike the stub these replaced, it has to SAY so rather than return
// quietly. A glCreateTextures name is a created object and gets past the name check.
TEST_F(TextureTest, CompressedTextureSubImage3DRejectsAGeneratedButNeverBoundName) {
    GLuint generated = 0;
    MG_Impl::GLImpl::GenTextures(1, &generated);
    ASSERT_NE(generated, 0u);
    DrainPendingGlErrors();

    Uint8 blocks[kRgtc1Size8x8x8] = {};
    MG_Impl::GLImpl::CompressedTextureSubImage3D(generated, 0, 0, 0, 0, 8, 8, 8, GL_COMPRESSED_RED_RGTC1,
                                                 kRgtc1Size8x8x8, blocks);
    ExpectSingleGlError(GL_INVALID_OPERATION);

    // A created name is past the name check, so whatever it answers is about the IMAGE (this one
    // holds none yet), never about the name.
    GLuint created = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_3D, 1, &created);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    MG_Impl::GLImpl::CompressedTextureSubImage3D(created, 0, 0, 0, 0, 8, 8, 8, GL_COMPRESSED_RED_RGTC1,
                                                 kRgtc1Size8x8x8, blocks);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_OPERATION)
        << "a created 3D texture with no compressed image is an image error, not a name error";
    DrainPendingGlErrors();
}

// Core GL defines no compressed format for a 1D target, so both the bound and the by-name entry
// point have to REFUSE the call. The by-name one used to be an exported no-op that raised nothing,
// which is the one answer an application cannot act on.
TEST_F(TextureTest, CompressedTextureSubImage1DRefusesLikeTheBoundCall) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_1D, texture);
    MG_Impl::GLImpl::TexImage1D(GL_TEXTURE_1D, 0, GL_R8, 8, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    DrainPendingGlErrors();

    Uint8 blocks[16] = {};
    MG_Impl::GLImpl::CompressedTexSubImage1D(GL_TEXTURE_1D, 0, 0, 8, GL_COMPRESSED_RED_RGTC1,
                                             static_cast<GLsizei>(sizeof(blocks)), blocks);
    ExpectSingleGlError(GL_INVALID_ENUM);

    MG_Impl::GLImpl::CompressedTextureSubImage1D(texture, 0, 0, 8, GL_COMPRESSED_RED_RGTC1,
                                                 static_cast<GLsizei>(sizeof(blocks)), blocks);
    ExpectSingleGlError(GL_INVALID_ENUM);
}

TEST_F(TextureTest, CompressedTexSubImage2DRejectsTheRegionsGLForbids) {
    const GLuint texture = MakeCompressedRgtc1Texture8x8();
    Uint8 blocks[kRgtc1Size8x8] = {};
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // A format that is not the one the image is stored in.
    MG_Impl::GLImpl::CompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 8, 8, GL_COMPRESSED_RG_RGTC2, 64, blocks);
    ExpectSingleGlError(GL_INVALID_OPERATION);

    // A start that is not on a block boundary.
    MG_Impl::GLImpl::CompressedTexSubImage2D(GL_TEXTURE_2D, 0, 2, 0, 4, 8, GL_COMPRESSED_RED_RGTC1, 16, blocks);
    ExpectSingleGlError(GL_INVALID_OPERATION);

    // A width that is neither a whole number of blocks nor a run to the image's edge.
    MG_Impl::GLImpl::CompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 2, 8, GL_COMPRESSED_RED_RGTC1, 16, blocks);
    ExpectSingleGlError(GL_INVALID_OPERATION);

    // A region that runs off the image.
    MG_Impl::GLImpl::CompressedTexSubImage2D(GL_TEXTURE_2D, 0, 4, 0, 8, 8, GL_COMPRESSED_RED_RGTC1, 32, blocks);
    ExpectSingleGlError(GL_INVALID_VALUE);

    // An imageSize that does not match the region.
    MG_Impl::GLImpl::CompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 8, 8, GL_COMPRESSED_RED_RGTC1, 16, blocks);
    ExpectSingleGlError(GL_INVALID_VALUE);

    // A format with no defined block layout here.
    MG_Impl::GLImpl::CompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 8, 8, GL_RGBA8, 32, blocks);
    ExpectSingleGlError(GL_INVALID_ENUM);

    // An uncompressed image has nothing for it to replace.
    GLuint plain = 0;
    MG_Impl::GLImpl::GenTextures(1, &plain);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, plain);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_R8, 8, 8, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    MG_Impl::GLImpl::CompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 8, 8, GL_COMPRESSED_RED_RGTC1, kRgtc1Size8x8,
                                             blocks);
    ExpectSingleGlError(GL_INVALID_OPERATION);
    (void)texture;
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

// Clean is not enough: per GL 4.6 core 8.8 that zero-sized reset has to DEALLOCATE the image,
// not define an empty one. gluStateReset runs it on both default multisample textures on every
// texture unit of a 3.2+ context, and a default texture left 'defined' afterwards stops being
// skipped by IsUndefinedDefaultTexture - it then joins the per-draw sync and bind passes on
// every unit the reset touched and reaches an ES glTexStorage*Multisample(..., 0, 0), which ES
// 3.1 8.19 rejects on every driver.
TEST_F(TextureTest, ZeroSizedMultisampleTexImageDeallocatesTheImage) {
    MG_Impl::GLImpl::ActiveTexture(GL_TEXTURE0);

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
    const auto& defaultMultisample = MG_State::pGLContext->GetTextureUnitObject(0)
                                         .GetBindingSlot(TextureTarget::Texture2DMultisample)
                                         .GetBoundObject();
    MG_Impl::GLImpl::TexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, 1, GL_RGBA8, 4, 4, GL_TRUE);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    ASSERT_FALSE(MG_State::GLState::IsUndefinedDefaultTexture(defaultMultisample.get()));

    MG_Impl::GLImpl::TexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, 1, GL_RGBA8, 0, 0, GL_TRUE);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_TRUE(MG_State::GLState::IsUndefinedDefaultTexture(defaultMultisample.get()));

    // The array target's reset also passes zero LAYERS, which deallocates just the same.
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D_MULTISAMPLE_ARRAY, 0);
    const auto& defaultMultisampleArray = MG_State::pGLContext->GetTextureUnitObject(0)
                                              .GetBindingSlot(TextureTarget::Texture2DMultisampleArray)
                                              .GetBoundObject();
    MG_Impl::GLImpl::TexImage3DMultisample(GL_TEXTURE_2D_MULTISAMPLE_ARRAY, 1, GL_RGBA8, 4, 4, 2, GL_TRUE);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    ASSERT_FALSE(MG_State::GLState::IsUndefinedDefaultTexture(defaultMultisampleArray.get()));

    MG_Impl::GLImpl::TexImage3DMultisample(GL_TEXTURE_2D_MULTISAMPLE_ARRAY, 1, GL_RGBA8, 4, 4, 0, GL_TRUE);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_TRUE(MG_State::GLState::IsUndefinedDefaultTexture(defaultMultisampleArray.get()));

    // The immutable forms do NOT share that leniency: GL 4.6 core 8.19 makes a size below 1
    // INVALID_VALUE, and freezing an imageless texture as immutable would be unrecoverable.
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D_MULTISAMPLE, texture);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    MG_Impl::GLImpl::TexStorage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, 1, GL_RGBA8, 0, 0, GL_TRUE);
    ExpectSingleGlError(GL_INVALID_VALUE);
    EXPECT_FALSE(MG_State::pGLContext->GetTextureUnitObject(0)
                     .GetBindingSlot(TextureTarget::Texture2DMultisample)
                     .GetBoundObject()
                     ->IsImmutable());

    MG_Impl::GLImpl::DeleteTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
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
        // Legacy <=8-bit-per-channel DESKTOP-ONLY formats store as UNorm8 component arrays, in the
        // 8-bit-per-channel ES format that layout already is. Storing them in the narrower
        // GL_RGB565/GL_RGBA4 they nominally fit in made the driver requantize the shadow bytes on
        // every upload, which is not lossless: 5-bit 2 -> UNorm8 16 -> 16/255*31 = 1.945, which a
        // truncating driver reads back as 1 (KHR-GL43.copy_image rgb4->rgb4, 12/12 failing on Mali).
        {GL_R3_G3_B2, GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE},
        {GL_RGB4, GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE},
        {GL_RGB5, GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE},
        {GL_RGBA2, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE},
        // The two that are ES formats in their own right keep their native storage: an application
        // that asks for GL_RGBA4 or GL_RGB5_A1 is asking for the smaller image, and the same
        // normalization also picks the storage for glRenderbufferStorage, where those two are
        // ordinary ES render targets rather than a desktop-compatibility shim.
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

// ---- GL_RGB9_E5 raw-preserving transfer --------------------------------------------------------
// RGB9_E5 packs three 9-bit mantissas against one shared 5-bit exponent, so a value has several
// legal encodings (shift the exponent up, shift every mantissa down). The spec's encode algorithm
// (GL 4.6 8.5.2) always emits the canonical one, which makes decode-to-float / re-encode
// value-preserving but NOT bit-preserving. glTexImage followed by glGetTexImage has to hand the
// application its own bits back, so a client (format, type) whose word already IS the storage word
// must move verbatim. GL CTS KHR-GL43.copy_image caught the round trip turning the uploaded
// 0xf8fc0000 into 0xe7e00000 ("CopyImageSubData modified contents of source image") and a copied-in
// 0x60000000 into 0x00000000 ("CopyImageSubData stored invalid data in copied region").

namespace {
    Uint32 RoundTripSharedExponentWord(Uint32 word) {
        Float rgb[3];
        MG_Util::DecodeSharedExponentRGB9E5(word, rgb);
        return MG_Util::EncodeSharedExponentRGB9E5(rgb);
    }
} // namespace

TEST(SharedExponentRGB9E5Test, EncodeReproducesCanonicalWordsExactly) {
    // Canonical encodings - the ones the spec algorithm emits - must survive a decode/encode round
    // trip untouched, or every conversion INTO RGB9_E5 would be off as well.
    const Uint32 canonical[] = {
        0x00000000u, // all zero
        0x0FFFFFFFu, // exponent 1, every mantissa saturated (smallest normalized exponent in use)
        0x000003FFu, // exponent 0: the denormal range, mantissas 511 / 1 / 0
        0x81010100u, // (1.0, 0.5, 0.25)
        0xE7E00000u, // (0, 0, 8064) - what the CTS round trip produced
        0xFFFFFFFFu, // exponent 31 with saturated mantissas = the largest representable texel
    };
    for (const Uint32 word : canonical) {
        EXPECT_EQ(RoundTripSharedExponentWord(word), word) << "word 0x" << std::hex << word;
        // Encoding is idempotent: a second pass may not drift either.
        EXPECT_EQ(RoundTripSharedExponentWord(RoundTripSharedExponentWord(word)), word);
    }
}

TEST(SharedExponentRGB9E5Test, EncodeCanonicalizesRedundantWords) {
    // The exact QPA signatures. Both pairs hold the same value, so the encoder is not wrong - which
    // is why the fix has to be a raw path rather than an encoder change.
    Float observed[3];
    MG_Util::DecodeSharedExponentRGB9E5(0xF8FC0000u, observed);
    Float canonical[3];
    MG_Util::DecodeSharedExponentRGB9E5(0xE7E00000u, canonical);
    EXPECT_EQ(observed[2], 8064.0f);
    EXPECT_EQ(canonical[2], 8064.0f);
    EXPECT_EQ(RoundTripSharedExponentWord(0xF8FC0000u), 0xE7E00000u);

    // Exponent 12 with all-zero mantissas is still the value zero, and canonicalizes to the
    // all-zero word.
    EXPECT_EQ(RoundTripSharedExponentWord(0x60000000u), 0x00000000u);
    // Mantissa 1 at exponent 1 renormalizes down into the denormal range.
    EXPECT_EQ(RoundTripSharedExponentWord(0x08000001u), 0x00000002u);
}

TEST(SharedExponentRGB9E5Test, RawPackedPixelTransferCoversOnlyIdenticalLayouts) {
    using MG_Util::PixelStoreProcessor::IsRawPackedPixelTransfer;

    // The four pairs whose client word is bit-identical to the packed storage word.
    EXPECT_TRUE(IsRawPackedPixelTransfer(TextureInternalFormat::RGB9E5, TextureInputFormat::RGB,
                                         TexturePixelDataType::UnsignedInt5999Rev));
    EXPECT_TRUE(IsRawPackedPixelTransfer(TextureInternalFormat::R11FG11FB10F, TextureInputFormat::RGB,
                                         TexturePixelDataType::UnsignedInt101111Rev));
    EXPECT_TRUE(IsRawPackedPixelTransfer(TextureInternalFormat::RGB10A2, TextureInputFormat::RGBA,
                                         TexturePixelDataType::UnsignedInt2101010Rev));
    EXPECT_TRUE(IsRawPackedPixelTransfer(TextureInternalFormat::RGB10A2UI, TextureInputFormat::RGBAInteger,
                                         TexturePixelDataType::UnsignedInt2101010Rev));

    // A different packed float layout of the same width is still a conversion.
    EXPECT_FALSE(IsRawPackedPixelTransfer(TextureInternalFormat::RGB9E5, TextureInputFormat::RGB,
                                          TexturePixelDataType::UnsignedInt101111Rev));
    EXPECT_FALSE(IsRawPackedPixelTransfer(TextureInternalFormat::R11FG11FB10F, TextureInputFormat::RGB,
                                          TexturePixelDataType::UnsignedInt5999Rev));
    // So is a component client type, or the same word against a component internal format.
    EXPECT_FALSE(IsRawPackedPixelTransfer(TextureInternalFormat::RGB9E5, TextureInputFormat::RGB,
                                          TexturePixelDataType::Float));
    EXPECT_FALSE(IsRawPackedPixelTransfer(TextureInternalFormat::RGB8, TextureInputFormat::RGB,
                                          TexturePixelDataType::UnsignedInt5999Rev));
    EXPECT_FALSE(IsRawPackedPixelTransfer(TextureInternalFormat::RGBA32F, TextureInputFormat::RGBA,
                                          TexturePixelDataType::UnsignedInt2101010Rev));
    // Integerness has to line up too: the normalized and integer 10/10/10/2 words are not the
    // same client layout even though they are the same bit field.
    EXPECT_FALSE(IsRawPackedPixelTransfer(TextureInternalFormat::RGB10A2, TextureInputFormat::RGBAInteger,
                                          TexturePixelDataType::UnsignedInt2101010Rev));
    EXPECT_FALSE(IsRawPackedPixelTransfer(TextureInternalFormat::RGB10A2UI, TextureInputFormat::RGBA,
                                          TexturePixelDataType::UnsignedInt2101010Rev));
    EXPECT_FALSE(IsRawPackedPixelTransfer(TextureInternalFormat::Unknown, TextureInputFormat::RGB,
                                          TexturePixelDataType::UnsignedInt5999Rev));
}

TEST(SharedExponentRGB9E5Test, RedundantPackedEncodingIsRGB9E5Only) {
    using MG_Util::PixelStoreProcessor::HasRedundantPackedEncoding;

    // This is the predicate that decides whether the CPU shadow has to answer glGetTexImage
    // instead of a GPU readback, so it must be as narrow as the defect: only the shared exponent
    // has several legal encodings of one value.
    EXPECT_TRUE(HasRedundantPackedEncoding(TextureInternalFormat::RGB9E5));

    // The other three packed 32-bit layouts round-trip through float32 bit-exactly (each field is
    // either an integer or a unique float encoding), so a GPU readback still serves them - which
    // matters because RGB10_A2 and R11F_G11F_B10F ARE colour-renderable and their shadow can
    // legitimately be stale.
    EXPECT_FALSE(HasRedundantPackedEncoding(TextureInternalFormat::RGB10A2));
    EXPECT_FALSE(HasRedundantPackedEncoding(TextureInternalFormat::RGB10A2UI));
    EXPECT_FALSE(HasRedundantPackedEncoding(TextureInternalFormat::R11FG11FB10F));

    // Nothing unpacked qualifies, and neither does an unknown format.
    EXPECT_FALSE(HasRedundantPackedEncoding(TextureInternalFormat::RGBA8));
    EXPECT_FALSE(HasRedundantPackedEncoding(TextureInternalFormat::RGBA32F));
    EXPECT_FALSE(HasRedundantPackedEncoding(TextureInternalFormat::RGB8));
    EXPECT_FALSE(HasRedundantPackedEncoding(TextureInternalFormat::Unknown));
}

TEST_F(TextureTest, TexImage2DRGB9E5KeepsNonCanonicalClientWords) {
    // Upload direction: GL_RGB / GL_UNSIGNED_INT_5_9_9_9_REV into GL_RGB9_E5 stores the client
    // words untouched, including the redundant encodings the CTS generates.
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);

    const Uint32 words[] = {0xF8FC0000u, 0x60000000u, 0x08000001u, 0x0FFFFFFFu};
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGB9_E5, 4, 1, 0, GL_RGB, GL_UNSIGNED_INT_5_9_9_9_REV, words);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    const auto* stored = GetBoundTexture2DLevelBytes(texture);
    ASSERT_NE(stored, nullptr);
    Uint32 readBack[4] = {};
    std::memcpy(readBack, stored, sizeof(readBack));
    for (Int i = 0; i < 4; ++i) {
        EXPECT_EQ(readBack[i], words[i]) << "texel " << i;
    }

    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, 0);
}

TEST_F(TextureTest, TexImage2DRGB9E5FromOtherPackedFloatTypeStillConverts) {
    // Negative control for the raw path: a genuinely different client layout keeps the
    // decode-to-float / re-encode conversion.
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);

    // 10F_11F_11F_REV word holding (1.0, 0.5, 0.25) - see the packed readback encode tests.
    const Uint32 packedFloatWord = 0x681C03C0u;
    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGB9_E5, 1, 1, 0, GL_RGB, GL_UNSIGNED_INT_10F_11F_11F_REV,
                                &packedFloatWord);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    const auto* stored = GetBoundTexture2DLevelBytes(texture);
    ASSERT_NE(stored, nullptr);
    Uint32 word = 0;
    std::memcpy(&word, stored, sizeof(word));
    const Float rgb[3] = {1.0f, 0.5f, 0.25f};
    EXPECT_EQ(word, MG_Util::EncodeSharedExponentRGB9E5(rgb));
    EXPECT_NE(word, packedFloatWord) << "the raw path must not swallow a real conversion";

    MG_Impl::GLImpl::PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, 0);
}

TEST_F(TextureTest, StorePackedWordsToClientCopiesWordsVerbatimUnderPackParams) {
    // Readback direction: the raw store copies the words bit-for-bit while still honoring the
    // client-side PACK addressing (alignment, skip rows/pixels) and GL_PACK_SWAP_BYTES.
    namespace ReadbackImpl = MG_Backend::DirectGLES::ReadbackImpl;

    const Uint32 source[] = {0xF8FC0000u, 0x60000000u, 0x08000001u,  // row 0
                             0x0FFFFFFFu, 0xFFFFFFFFu, 0x00000000u}; // row 1
    constexpr Uint32 kFill = 0xDEADBEEFu;
    Uint32 destination[16];
    std::fill(std::begin(destination), std::end(destination), kFill);

    MG_Impl::GLImpl::PixelStorei(GL_PACK_ALIGNMENT, 8); // rows of 3 words (12 B) pad to 16 B
    MG_Impl::GLImpl::PixelStorei(GL_PACK_SKIP_ROWS, 1);
    MG_Impl::GLImpl::PixelStorei(GL_PACK_SKIP_PIXELS, 1);
    ASSERT_TRUE(ReadbackImpl::StorePackedWordsToClient(reinterpret_cast<const Uint8*>(source), /*width=*/3,
                                                       /*sliceHeight=*/2, /*sliceCount=*/1,
                                                       GL_UNSIGNED_INT_5_9_9_9_REV, destination,
                                                       /*applyPackImageParams=*/false));
    // Row 0 lands at SKIP_ROWS * 16 + SKIP_PIXELS * 4 = 20 bytes = word 5; row 1 one 16-byte
    // stride further along, at word 9.
    for (Int i = 0; i < 3; ++i) {
        EXPECT_EQ(destination[5 + i], source[i]) << "row 0 texel " << i;
        EXPECT_EQ(destination[9 + i], source[3 + i]) << "row 1 texel " << i;
    }
    // The skipped region and the row padding stay untouched.
    EXPECT_EQ(destination[0], kFill);
    EXPECT_EQ(destination[4], kFill);
    EXPECT_EQ(destination[8], kFill);
    EXPECT_EQ(destination[12], kFill);

    // GL_PACK_SWAP_BYTES reverses each 4-byte word.
    std::fill(std::begin(destination), std::end(destination), kFill);
    MG_Impl::GLImpl::PixelStorei(GL_PACK_SKIP_ROWS, 0);
    MG_Impl::GLImpl::PixelStorei(GL_PACK_SKIP_PIXELS, 0);
    MG_Impl::GLImpl::PixelStorei(GL_PACK_ALIGNMENT, 1);
    MG_Impl::GLImpl::PixelStorei(GL_PACK_SWAP_BYTES, GL_TRUE);
    ASSERT_TRUE(ReadbackImpl::StorePackedWordsToClient(reinterpret_cast<const Uint8*>(source), /*width=*/3,
                                                       /*sliceHeight=*/1, /*sliceCount=*/1,
                                                       GL_UNSIGNED_INT_5_9_9_9_REV, destination,
                                                       /*applyPackImageParams=*/false));
    EXPECT_EQ(destination[0], 0x0000FCF8u); // byte-reversed 0xF8FC0000
    EXPECT_EQ(destination[1], 0x00000060u);

    MG_Impl::GLImpl::PixelStorei(GL_PACK_SWAP_BYTES, GL_FALSE);
    MG_Impl::GLImpl::PixelStorei(GL_PACK_ALIGNMENT, 4);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// GL 4.6 core table 23.18: GL_TEXTURE_COMPARE_FUNC takes the whole eight-function depth-compare
// range. The validator used to start it at GL_LEQUAL, which sits in the middle of the contiguous
// GL_NEVER..GL_ALWAYS block, so NEVER/LESS/EQUAL were rejected while GREATER/NOTEQUAL/GEQUAL only
// got through because they happen to be numerically above LEQUAL.
TEST_F(TextureTest, SamplerCompareFuncAcceptsTheWholeNeverToAlwaysRange) {
    GLuint sampler = 0;
    MG_Impl::GLImpl::GenSamplers(1, &sampler);
    ASSERT_NE(sampler, 0u);

    const GLenum compareFuncs[] = {GL_NEVER,   GL_LESS,     GL_EQUAL,  GL_LEQUAL,
                                   GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
    for (GLenum func : compareFuncs) {
        MG_Impl::GLImpl::SamplerParameteri(sampler, GL_TEXTURE_COMPARE_FUNC, static_cast<GLint>(func));
        EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR) << "compare func " << func << " was rejected";
        GLint readBack = 0;
        MG_Impl::GLImpl::GetSamplerParameteriv(sampler, GL_TEXTURE_COMPARE_FUNC, &readBack);
        EXPECT_EQ(static_cast<GLenum>(readBack), func);
        EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    }

    // Just outside the block on both sides is still INVALID_ENUM.
    MG_Impl::GLImpl::SamplerParameteri(sampler, GL_TEXTURE_COMPARE_FUNC, GL_NEVER - 1);
    ExpectSingleGlError(GL_INVALID_ENUM);
    MG_Impl::GLImpl::SamplerParameteri(sampler, GL_TEXTURE_COMPARE_FUNC, GL_ALWAYS + 1);
    ExpectSingleGlError(GL_INVALID_ENUM);

    MG_Impl::GLImpl::DeleteSamplers(1, &sampler);
}

// GL 4.6 core table 23.19: GL_TEXTURE_BINDING_* and GL_SAMPLER_BINDING are per-texture-unit, so
// glGetIntegeri_v must answer for unit `index` - not fall through to the backend, which knows
// nothing about the frontend's binding state.
TEST_F(TextureTest, GetIntegeriVReportsPerUnitTextureAndSamplerBindings) {
    GLuint textures[2] = {0, 0};
    MG_Impl::GLImpl::GenTextures(2, textures);
    ASSERT_NE(textures[0], 0u);
    ASSERT_NE(textures[1], 0u);

    MG_Impl::GLImpl::ActiveTexture(GL_TEXTURE0);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, textures[0]);
    MG_Impl::GLImpl::ActiveTexture(GL_TEXTURE3);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, textures[1]);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    GLint binding = -1;
    MG_Impl::GLImpl::GetIntegeri_v(GL_TEXTURE_BINDING_2D, 0, &binding);
    EXPECT_EQ(static_cast<GLuint>(binding), textures[0]);
    MG_Impl::GLImpl::GetIntegeri_v(GL_TEXTURE_BINDING_2D, 3, &binding);
    EXPECT_EQ(static_cast<GLuint>(binding), textures[1]);
    // An unbound unit reports 0, and a target nothing was bound to reports 0 as well.
    MG_Impl::GLImpl::GetIntegeri_v(GL_TEXTURE_BINDING_2D, 2, &binding);
    EXPECT_EQ(binding, 0);
    MG_Impl::GLImpl::GetIntegeri_v(GL_TEXTURE_BINDING_3D, 0, &binding);
    EXPECT_EQ(binding, 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // The non-indexed query keeps reporting the ACTIVE unit, which is still unit 3.
    GLint activeUnitBinding = -1;
    MG_Impl::GLImpl::GetIntegerv(GL_TEXTURE_BINDING_2D, &activeUnitBinding);
    EXPECT_EQ(static_cast<GLuint>(activeUnitBinding), textures[1]);

    GLuint sampler = 0;
    MG_Impl::GLImpl::GenSamplers(1, &sampler);
    ASSERT_NE(sampler, 0u);
    MG_Impl::GLImpl::BindSampler(2, sampler);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    MG_Impl::GLImpl::GetIntegeri_v(GL_SAMPLER_BINDING, 2, &binding);
    EXPECT_EQ(static_cast<GLuint>(binding), sampler);
    MG_Impl::GLImpl::GetIntegeri_v(GL_SAMPLER_BINDING, 1, &binding);
    EXPECT_EQ(binding, 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // Out of range is INVALID_VALUE, not a backend passthrough.
    GLint maxUnits = 0;
    MG_Impl::GLImpl::GetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &maxUnits);
    ASSERT_GT(maxUnits, 0);
    MG_Impl::GLImpl::GetIntegeri_v(GL_TEXTURE_BINDING_2D, static_cast<GLuint>(maxUnits) + 1024u, &binding);
    ExpectSingleGlError(GL_INVALID_VALUE);

    MG_Impl::GLImpl::BindSampler(2, 0);
    MG_Impl::GLImpl::DeleteSamplers(1, &sampler);
    MG_Impl::GLImpl::ActiveTexture(GL_TEXTURE0);
    MG_Impl::GLImpl::DeleteTextures(2, textures);
    DrainPendingGlErrors();
}

// glGetFloati_v / glGetDoublei_v were no-op stubs: they left the caller's buffer holding whatever
// was on the stack. They are converters over the integer indexed query.
TEST_F(TextureTest, GetFloatiVAndGetDoubleiVConvertTheIndexedIntegerQuery) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    ASSERT_NE(texture, 0u);
    MG_Impl::GLImpl::ActiveTexture(GL_TEXTURE1);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    GLfloat asFloat = -1.0f;
    MG_Impl::GLImpl::GetFloati_v(GL_TEXTURE_BINDING_2D, 1, &asFloat);
    EXPECT_FLOAT_EQ(asFloat, static_cast<GLfloat>(texture));

    GLdouble asDouble = -1.0;
    MG_Impl::GLImpl::GetDoublei_v(GL_TEXTURE_BINDING_2D, 1, &asDouble);
    EXPECT_DOUBLE_EQ(asDouble, static_cast<GLdouble>(texture));
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::ActiveTexture(GL_TEXTURE0);
    MG_Impl::GLImpl::DeleteTextures(1, &texture);
    DrainPendingGlErrors();
}

// GL 3.3 core 3.8.2: the unit glBindSampler accepts is bounded by
// GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS. The gate read the frontend's MAX_TEXTURE_IMAGE_UNITS
// instead - the capacity of the unit array, 192 - so every unit the backend does not have was
// accepted, and the single-bind path disagreed with the multi-bind twin about where the units end.
// The backend is stood in so the two limits are distinguishable no matter what the real one
// advertises.
TEST_F(TextureTest, BindSamplerRejectsUnitsBeyondMaxCombinedTextureImageUnits) {
    GLuint sampler = 0;
    MG_Impl::GLImpl::GenSamplers(1, &sampler);
    ASSERT_NE(sampler, 0u);

    constexpr GLint kCombinedUnits = 24;
    static_assert(kCombinedUnits < MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS,
                  "the stand-in limit has to be below the unit array capacity to tell the two apart");
    auto backend = MakeUnique<FormatCapabilityBackend>();
    FormatCapabilityBackend::MutableDynamicParameters().MaxCombinedTextureImageUnits = kCombinedUnits;
    ScopedBackendOverride backendOverride(Move(backend));

    GLint reportedUnits = 0;
    MG_Impl::GLImpl::GetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &reportedUnits);
    ASSERT_EQ(reportedUnits, kCombinedUnits);

    // The last unit that exists still binds.
    const GLuint lastUnit = static_cast<GLuint>(kCombinedUnits - 1);
    MG_Impl::GLImpl::BindSampler(lastUnit, sampler);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_NE(MG_State::pGLContext->GetTextureUnitObject(static_cast<Int>(lastUnit)).GetSamplerObject(), nullptr);

    // One past it does not - this is the unit the old gate accepted.
    MG_Impl::GLImpl::BindSampler(static_cast<GLuint>(kCombinedUnits), sampler);
    ExpectSingleGlError(GL_INVALID_VALUE);
    EXPECT_EQ(MG_State::pGLContext->GetTextureUnitObject(kCombinedUnits).GetSamplerObject(), nullptr);

    // Past the unit array as well is the same error, not an out-of-bounds index.
    MG_Impl::GLImpl::BindSampler(
        static_cast<GLuint>(MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS) + 4u, sampler);
    ExpectSingleGlError(GL_INVALID_VALUE);

    // Both gates now read the same limit: a multi-bind that ends exactly at it binds, and one that
    // runs a single unit past it is the multi-bind's INVALID_OPERATION, reported up front - not the
    // single-bind INVALID_VALUE from somewhere inside the loop.
    const GLuint samplers[2] = {sampler, sampler};
    MG_Impl::GLImpl::BindSamplers(static_cast<GLuint>(kCombinedUnits - 2), 2, samplers);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    MG_Impl::GLImpl::BindSamplers(lastUnit, 2, samplers);
    ExpectSingleGlError(GL_INVALID_OPERATION);

    MG_Impl::GLImpl::BindSampler(lastUnit, 0);
    MG_Impl::GLImpl::BindSampler(static_cast<GLuint>(kCombinedUnits - 2), 0);
    MG_Impl::GLImpl::DeleteSamplers(1, &sampler);
    DrainPendingGlErrors();
}

// The DSA by-name entry points are emulated by temporarily binding the named texture onto the
// active unit's slot for its target, running the classic bound-texture code, then putting the
// previous binding back. For as long as the emulated call runs, that swap is a REAL change to
// which texture is bound at that unit, so both transitions have to move the texture bind
// generation.
//
// They used to move nothing. Backends memoise per-unit work keyed on the bind generation and
// BORROW the binding slot (they hold a pointer to the slot's shared_ptr, not a copy), so a memo
// built while texture A sat in the slot stayed "valid" while B was temporarily in it - and the
// backend then drove A's backend twin from B's frontend state, re-specifying A's backend storage
// with B's shape. Any content A only ever had on the GPU was gone. That is what blanked
// Minecraft's lightmap when Iris uploaded to a BSL shadow map: the text shader multiplies by the
// lightmap, so `if (color.a < 0.1) discard` then threw away every glyph in the process.
TEST_F(TextureTest, NamedTextureCallKeepsUnitBindingAccountingCoherent) {
    GLuint names[2] = {};
    MG_Impl::GLImpl::GenTextures(2, names);
    const GLuint boundName = names[0];
    const GLuint namedName = names[1];

    MG_Impl::GLImpl::ActiveTexture(GL_TEXTURE0);
    // Instantiate both as 2D objects, then leave `boundName` on the unit.
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, namedName);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, boundName);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    auto& slot = MG_State::pGLContext->GetTextureUnitObject(0).GetBindingSlot(TextureTarget::Texture2D);
    const auto boundObject = slot.GetBoundObject();
    ASSERT_NE(boundObject, nullptr);
    ASSERT_EQ(boundObject->GetExternalIndex(), boundName);

    // TextureParameteriv is one of the by-name calls that is emulated by binding: it reaches
    // WithTemporarilyBoundNamedTexture, unlike the scalar TextureParameteri, which edits the
    // object directly and never touches a unit.
    const Uint64 base = MG_State::pGLContext->GetTextureBindGeneration();
    const GLint maxLevel = 0;
    MG_Impl::GLImpl::TextureParameteriv(namedName, GL_TEXTURE_MAX_LEVEL, &maxLevel);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // The emulation put `namedName` on the unit and took it off again. A generation-keyed memo
    // must be able to see that the slot it borrows was not stable across the call.
    EXPECT_GT(MG_State::pGLContext->GetTextureBindGeneration(), base)
        << "a by-name texture call swapped a live unit binding without moving the bind generation";
    // ...and the application-visible binding is exactly what it was before the call.
    EXPECT_EQ(slot.GetBoundObject(), boundObject);

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, 0);
    MG_Impl::GLImpl::DeleteTextures(2, names);
    DrainPendingGlErrors();
}

// ---- Three-channel colour-renderable widening (Complementary Reimagined / Iris) ----------------
//
// No real OpenGL ES driver renders to a three-channel image, so a colour attachment the
// application asked for as GL_RGB8_SNORM or GL_RGB16F has to be stored in the four-channel
// sibling. The bit that says so used to be reachable for multisample storage only, which is why
// an ordinary GL_TEXTURE_2D attachment in one of those formats had no fallback at all and the
// frontend could only answer GL_FRAMEBUFFER_UNSUPPORTED.

TEST_F(TextureTest, ColorAttachableTargetsRequestTheThreeChannelWidening) {
    using MobileGL::MG_Backend::DirectGLES::TextureImpl::GetRenderTargetNormalizeOptions;
    using MobileGL::MG_Backend::DirectGLES::TextureImpl::TargetRequiresRenderableFormat;

    MG_External::GLESCapabilities capabilities{};
    capabilities.SupportsRenderSnorm = true;
    capabilities.SupportsNorm16Texture = true;

    // Every image that can be a colour attachment, not just the multisample pair: an ordinary 2D
    // texture is what Iris attaches, and it used to be excluded.
    for (const TextureTarget target : {TextureTarget::Texture2D, TextureTarget::Texture3D,
                                       TextureTarget::TextureCubeMap, TextureTarget::Texture2DArray,
                                       TextureTarget::TextureCubeMapArray, TextureTarget::Texture2DMultisample,
                                       TextureTarget::Texture2DMultisampleArray, TextureTarget::Texture1D,
                                       TextureTarget::Texture1DArray, TextureTarget::TextureRectangle}) {
        const SizeT targetIndex = MobileGL::MG_Backend::GetFormatCapabilityTargetIndex(target);
        EXPECT_TRUE(TargetRequiresRenderableFormat(targetIndex))
            << "target " << MG_Util::ConvertTextureTargetToString(target);
        EXPECT_TRUE(GetRenderTargetNormalizeOptions(capabilities, targetIndex) &
                    PixelFormatNormalizeOptionBit::NoThreeChannelRenderTarget)
            << "target " << MG_Util::ConvertTextureTargetToString(target);
    }
    // A renderbuffer exists only to be attached.
    EXPECT_TRUE(TargetRequiresRenderableFormat(MobileGL::MG_Backend::GetRenderbufferFormatCapabilityTargetIndex()));

    // A buffer texture is the one image that can never be an attachment; its storage belongs to
    // the buffer object, so widening it would misdescribe the application's data.
    const SizeT bufferIndex = MobileGL::MG_Backend::GetFormatCapabilityTargetIndex(TextureTarget::TextureBuffer);
    EXPECT_FALSE(TargetRequiresRenderableFormat(bufferIndex));
    EXPECT_FALSE(GetRenderTargetNormalizeOptions(capabilities, bufferIndex));

    // Without EXT_render_snorm a 16-bit SNORM render target cannot keep its encoding either.
    MG_External::GLESCapabilities noSnormCapabilities{};
    const SizeT texture2DIndex = MobileGL::MG_Backend::GetFormatCapabilityTargetIndex(TextureTarget::Texture2D);
    EXPECT_TRUE(GetRenderTargetNormalizeOptions(noSnormCapabilities, texture2DIndex) &
                PixelFormatNormalizeOptionBit::NoSnorm16RenderTarget);
    EXPECT_FALSE(GetRenderTargetNormalizeOptions(capabilities, texture2DIndex) &
                 PixelFormatNormalizeOptionBit::NoSnorm16RenderTarget);

    // ...and neither can an 8-bit one. That half of the answer used to be missing entirely, which
    // is why an R8_SNORM / RG8_SNORM colour attachment got no substitute at all on a driver
    // without EXT_render_snorm.
    EXPECT_TRUE(GetRenderTargetNormalizeOptions(noSnormCapabilities, texture2DIndex) &
                PixelFormatNormalizeOptionBit::NoSnorm8RenderTarget);
    EXPECT_FALSE(GetRenderTargetNormalizeOptions(capabilities, texture2DIndex) &
                 PixelFormatNormalizeOptionBit::NoSnorm8RenderTarget);
    EXPECT_FALSE(GetRenderTargetNormalizeOptions(noSnormCapabilities, bufferIndex) &
                 PixelFormatNormalizeOptionBit::NoSnorm8RenderTarget);

    // 8-bit signed-normalized storage is core ES, so only EXT_render_snorm gates the 8-bit bit;
    // the 16-bit one also needs EXT_texture_norm16 for the encoding to exist at all.
    MG_External::GLESCapabilities noNorm16Capabilities{};
    noNorm16Capabilities.SupportsRenderSnorm = true;
    noNorm16Capabilities.SupportsNorm16Texture = false;
    EXPECT_TRUE(GetRenderTargetNormalizeOptions(noNorm16Capabilities, texture2DIndex) &
                PixelFormatNormalizeOptionBit::NoSnorm16RenderTarget);
    EXPECT_FALSE(GetRenderTargetNormalizeOptions(noNorm16Capabilities, texture2DIndex) &
                 PixelFormatNormalizeOptionBit::NoSnorm8RenderTarget);
}

// ---- Signed-normalized colour-renderable substitution (KHR-GL4x.texture_swizzle on Mali) -------
//
// A driver without GL_EXT_render_snorm treats every signed-normalized format as texture-only, so a
// colour attachment in one of them leaves the ES framebuffer incomplete: the draw lands nowhere and
// the readback falls through to the CPU shadow, which for a glTexImage2D(..., nullptr) output
// texture is all zeroes. The render-target bits used to reach GL_RGB16_SNORM alone, so five of the
// eight SNORM formats - and in particular the single-channel GL_R8_SNORM / GL_R16_SNORM that
// KHR-GL4x.texture_swizzle renders into for EVERY SNORM source format - had no fallback at all.

TEST_F(TextureTest, SnormRenderTargetOptionsApplyToEverySignedNormalizedFormat) {
    using MG_Util::TextureFormatProcessor::GetApplicablePixelFormatNormalizeOptions;
    const Flags<PixelFormatNormalizeOptionBit> requested =
        PixelFormatNormalizeOptionBit::NoSnorm8RenderTarget | PixelFormatNormalizeOptionBit::NoSnorm16RenderTarget;

    for (const GLenum internalFormat : {GL_R8_SNORM, GL_RG8_SNORM, GL_RGB8_SNORM, GL_RGBA8_SNORM}) {
        const auto applicable = GetApplicablePixelFormatNormalizeOptions(internalFormat, requested);
        EXPECT_TRUE(applicable & PixelFormatNormalizeOptionBit::NoSnorm8RenderTarget)
            << "internalformat 0x" << std::hex << internalFormat;
        // The two bits are per precision class, so the 16-bit one never reaches an 8-bit format -
        // that is what keeps the fallback reason from naming both.
        EXPECT_FALSE(applicable & PixelFormatNormalizeOptionBit::NoSnorm16RenderTarget)
            << "internalformat 0x" << std::hex << internalFormat;
    }
    for (const GLenum internalFormat : {GL_R16_SNORM, GL_RG16_SNORM, GL_RGB16_SNORM, GL_RGBA16_SNORM}) {
        const auto applicable = GetApplicablePixelFormatNormalizeOptions(internalFormat, requested);
        EXPECT_TRUE(applicable & PixelFormatNormalizeOptionBit::NoSnorm16RenderTarget)
            << "internalformat 0x" << std::hex << internalFormat;
        EXPECT_FALSE(applicable & PixelFormatNormalizeOptionBit::NoSnorm8RenderTarget)
            << "internalformat 0x" << std::hex << internalFormat;
    }
    // GL_RGB16_SNORM used to be granted the 16-bit bit only when the three-channel widening was
    // requested alongside it, which made the answer depend on the order the caller assembled its
    // option set in. The capability probe and the runtime storage choice assemble different sets.
    EXPECT_TRUE(GetApplicablePixelFormatNormalizeOptions(GL_RGB16_SNORM,
                                                         PixelFormatNormalizeOptionBit::NoSnorm16RenderTarget) &
                PixelFormatNormalizeOptionBit::NoSnorm16RenderTarget);

    // Nothing else responds to either bit; an unsigned-normalized or float format keeps its storage.
    for (const GLenum internalFormat : {GL_R8, GL_R16, GL_RGBA8, GL_RGBA16, GL_RGB16F, GL_RGBA32F, GL_RGB9_E5}) {
        EXPECT_FALSE(GetApplicablePixelFormatNormalizeOptions(internalFormat, requested))
            << "internalformat 0x" << std::hex << internalFormat;
    }
}

TEST_F(TextureTest, SnormRenderTargetSubstitutesKeepEveryChannelValueExactly) {
    using MG_Util::TextureFormatProcessor::NormalizePixelFormat;
    struct Case {
        GLenum requested;
        Flags<PixelFormatNormalizeOptionBit> options;
        GLenum internalFormat;
        GLenum format;
        GLenum type;
    };
    const Flags<PixelFormatNormalizeOptionBit> snorm8RT = PixelFormatNormalizeOptionBit::NoSnorm8RenderTarget;
    const Flags<PixelFormatNormalizeOptionBit> snorm16RT = PixelFormatNormalizeOptionBit::NoSnorm16RenderTarget;

    const Case cases[] = {
        // 8-bit: a half float represents every v/127 exactly (the worst case, -123/127, quantizes
        // 0.03 of a SNORM step away), so it is the same storage GL_RGBA8_SNORM already always got.
        {GL_R8_SNORM, snorm8RT, GL_R16F, GL_RED, GL_FLOAT},
        {GL_RG8_SNORM, snorm8RT, GL_RG16F, GL_RG, GL_FLOAT},
        {GL_RGBA8_SNORM, snorm8RT, GL_RGBA16F, GL_RGBA, GL_FLOAT},
        // 16-bit: NOT a half float. Its spacing just below 1.0 is some 16 SNORM steps, so it hands
        // -23451/32767 back as -23457 against a conformance window of one step; a 32-bit float
        // round-trips all 65535 channel values.
        {GL_R16_SNORM, snorm16RT, GL_R32F, GL_RED, GL_FLOAT},
        {GL_RG16_SNORM, snorm16RT, GL_RG32F, GL_RG, GL_FLOAT},
        {GL_RGBA16_SNORM, snorm16RT, GL_RGBA32F, GL_RGBA, GL_FLOAT},
        // The render-target bit outranks the narrower fallbacks, whichever way the caller's option
        // set was assembled: the capability probe folds the driver options in, the runtime storage
        // choice can see the render-target bit alone, and the two have to pick the same storage.
        {GL_R16_SNORM, snorm16RT | PixelFormatNormalizeOptionBit::NoNorm16, GL_R32F, GL_RED, GL_FLOAT},
        {GL_RG16_SNORM, snorm16RT | PixelFormatNormalizeOptionBit::NoSnorm16, GL_RG32F, GL_RG, GL_FLOAT},
        {GL_RGBA16_SNORM,
         snorm16RT | PixelFormatNormalizeOptionBit::NoNorm16 | PixelFormatNormalizeOptionBit::NoSnorm16,
         GL_RGBA32F, GL_RGBA, GL_FLOAT},
        // The three-channel formats go on through the widening, which outranks everything.
        {GL_RGB8_SNORM, snorm8RT | PixelFormatNormalizeOptionBit::NoThreeChannelRenderTarget, GL_RGBA16F, GL_RGBA,
         GL_FLOAT},
        {GL_RGB16_SNORM, snorm16RT | PixelFormatNormalizeOptionBit::NoThreeChannelRenderTarget, GL_RGBA32F, GL_RGBA,
         GL_FLOAT},
        // Control: with EXT_render_snorm neither bit is ever set, so the driver that renders to the
        // signed-normalized encoding keeps storing it byte for byte. This is the shape Adreno and
        // llvmpipe take, which is why the substitution is invisible on every gate the project runs.
        {GL_R8_SNORM, PixelFormatNormalizeOptionBit::None, GL_R8_SNORM, GL_RED, GL_BYTE},
        {GL_RG8_SNORM, PixelFormatNormalizeOptionBit::None, GL_RG8_SNORM, GL_RG, GL_BYTE},
        {GL_R16_SNORM, PixelFormatNormalizeOptionBit::None, GL_R16_SNORM, GL_RED, GL_SHORT},
        {GL_RG16_SNORM, PixelFormatNormalizeOptionBit::None, GL_RG16_SNORM, GL_RG, GL_SHORT},
        {GL_RGBA16_SNORM, PixelFormatNormalizeOptionBit::None, GL_RGBA16_SNORM, GL_RGBA, GL_SHORT},
        // ...and the bit for the other precision class does nothing on its own.
        {GL_R8_SNORM, snorm16RT, GL_R8_SNORM, GL_RED, GL_BYTE},
        {GL_R16_SNORM, snorm8RT, GL_R16_SNORM, GL_RED, GL_SHORT},
    };

    for (const auto& testCase : cases) {
        GLenum internalFormat = 0;
        GLenum format = 0;
        GLenum type = 0;
        NormalizePixelFormat(testCase.requested, testCase.options, &internalFormat, &format, &type);
        EXPECT_EQ(internalFormat, testCase.internalFormat) << "requested 0x" << std::hex << testCase.requested;
        EXPECT_EQ(format, testCase.format) << "requested 0x" << std::hex << testCase.requested;
        EXPECT_EQ(type, testCase.type) << "requested 0x" << std::hex << testCase.requested;
    }
}

TEST_F(TextureTest, ThreeChannelRenderTargetOptionAppliesToEveryDeniedThreeChannelFormat) {
    using MG_Util::TextureFormatProcessor::GetApplicablePixelFormatNormalizeOptions;
    const Flags<PixelFormatNormalizeOptionBit> requested =
        PixelFormatNormalizeOptionBit::NoThreeChannelRenderTarget;

    // GL_RGB16F in particular matched no case at all, so no option could ever apply to it and it
    // fell through NormalizePixelFormat's default passthrough unchanged.
    for (const GLenum internalFormat : {GL_RGB8_SNORM, GL_RGB16_SNORM, GL_RGB16, GL_RGB10, GL_RGB12, GL_RGB16F,
                                        GL_RGB32F, GL_SRGB8, GL_RGB8I, GL_RGB8UI, GL_RGB16I, GL_RGB16UI, GL_RGB32I,
                                        GL_RGB32UI}) {
        EXPECT_TRUE(GetApplicablePixelFormatNormalizeOptions(internalFormat, requested) &
                    PixelFormatNormalizeOptionBit::NoThreeChannelRenderTarget)
            << "internalformat 0x" << std::hex << internalFormat;
    }

    // Four-channel and shared-exponent formats are not widened: RGBA8_SNORM has its own always-on
    // fallback, and GL_RGB9_E5 has no four-channel sibling that would not need the shared exponent
    // unpacked on every transfer (nothing renders to it on desktop GL either).
    for (const GLenum internalFormat : {GL_RGBA8_SNORM, GL_RGBA16F, GL_RGBA8, GL_RGB8, GL_RGB9_E5}) {
        EXPECT_FALSE(GetApplicablePixelFormatNormalizeOptions(internalFormat, requested) &
                     PixelFormatNormalizeOptionBit::NoThreeChannelRenderTarget)
            << "internalformat 0x" << std::hex << internalFormat;
    }
}

TEST_F(TextureTest, ThreeChannelWideningRetargetsInternalFormatAndTransferPairTogether) {
    using MG_Util::TextureFormatProcessor::NormalizePixelFormat;
    struct Case {
        GLenum requested;
        Flags<PixelFormatNormalizeOptionBit> options;
        GLenum internalFormat;
        GLenum format;
        GLenum type;
    };
    const Flags<PixelFormatNormalizeOptionBit> widen = PixelFormatNormalizeOptionBit::NoThreeChannelRenderTarget;
    const Flags<PixelFormatNormalizeOptionBit> widenNoSnorm16 =
        PixelFormatNormalizeOptionBit::NoThreeChannelRenderTarget |
        PixelFormatNormalizeOptionBit::NoSnorm16RenderTarget;
    const Flags<PixelFormatNormalizeOptionBit> widenNoNorm16 =
        PixelFormatNormalizeOptionBit::NoThreeChannelRenderTarget | PixelFormatNormalizeOptionBit::NoNorm16;

    const Case cases[] = {
        // Complementary's colortex1 and colortex2. The transfer pair used to stay three-channel
        // and keep the *source* component type, emitting (GL_RGBA16F, GL_RGB, GL_BYTE) - which ES
        // rejects for glTexImage2D outright, and which only went unnoticed because the bit was
        // reachable for multisample storage alone (glTexStorage*Multisample takes no pair).
        {GL_RGB8_SNORM, widen, GL_RGBA16F, GL_RGBA, GL_FLOAT},
        {GL_RGB16F, widen, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT},
        {GL_RGB32F, widen, GL_RGBA32F, GL_RGBA, GL_FLOAT},
        // 16-bit SNORM keeps its encoding where EXT_render_snorm can render to it; a half float's
        // 11-bit mantissa cannot represent a 16-bit SNORM channel exactly, so the driver that
        // cannot render to the encoding gets the 32-bit float rather than the half.
        {GL_RGB16_SNORM, widen, GL_RGBA16_SNORM, GL_RGBA, GL_SHORT},
        {GL_RGB16_SNORM, widenNoSnorm16, GL_RGBA32F, GL_RGBA, GL_FLOAT},
        // 16-bit UNORM and the legacy 10/12-bit formats stored as RGB16. The same-width sibling
        // whenever the driver has EXT_texture_norm16 - which is what keeps the whole 48-bit
        // ARB_texture_view class on one ES view class, so a GL_RGB16 texture can be viewed as
        // GL_RGB16UI - and the 32-bit float only when it does not.
        {GL_RGB16, widen, GL_RGBA16, GL_RGBA, GL_UNSIGNED_SHORT},
        {GL_RGB10, widen, GL_RGBA16, GL_RGBA, GL_UNSIGNED_SHORT},
        {GL_RGB12, widen, GL_RGBA16, GL_RGBA, GL_UNSIGNED_SHORT},
        {GL_RGB16, widenNoNorm16, GL_RGBA32F, GL_RGBA, GL_FLOAT},
        {GL_RGB10, widenNoNorm16, GL_RGBA32F, GL_RGBA, GL_FLOAT},
        {GL_RGB12, widenNoNorm16, GL_RGBA32F, GL_RGBA, GL_FLOAT},
        // sRGB and the integer formats: the base format has to move to the four-channel one of the
        // right class, GL_RGBA_INTEGER included.
        {GL_SRGB8, widen, GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE},
        {GL_RGB8I, widen, GL_RGBA8I, GL_RGBA_INTEGER, GL_BYTE},
        {GL_RGB8UI, widen, GL_RGBA8UI, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE},
        {GL_RGB16I, widen, GL_RGBA16I, GL_RGBA_INTEGER, GL_SHORT},
        {GL_RGB16UI, widen, GL_RGBA16UI, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT},
        {GL_RGB32I, widen, GL_RGBA32I, GL_RGBA_INTEGER, GL_INT},
        {GL_RGB32UI, widen, GL_RGBA32UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT},
        // The widening outranks the other fallbacks, which all pick a three-channel storage the
        // driver still refuses to render to (GL_RGB8_SNORM -> GL_RGB16F, GL_RGB16 -> GL_RGB32F).
        {GL_RGB8_SNORM, widen | PixelFormatNormalizeOptionBit::NoSnorm8, GL_RGBA16F, GL_RGBA, GL_FLOAT},
        {GL_RGB16, widen | PixelFormatNormalizeOptionBit::NoNorm16, GL_RGBA32F, GL_RGBA, GL_FLOAT},
        // Control: without the bit nothing moves. The bit is only ever set for a target whose
        // native probe failed, so this is the shape every driver that does render to the
        // three-channel form keeps - per format, not per platform (llvmpipe renders to GL_RGB16F
        // but not to GL_RGB8_SNORM, GL_SRGB8, GL_RGB32F or the RGB integer formats).
        {GL_RGB8_SNORM, PixelFormatNormalizeOptionBit::None, GL_RGB8_SNORM, GL_RGB, GL_BYTE},
        {GL_RGB16F, PixelFormatNormalizeOptionBit::None, GL_RGB16F, GL_RGB, GL_HALF_FLOAT},
        {GL_RGB32F, PixelFormatNormalizeOptionBit::None, GL_RGB32F, GL_RGB, GL_FLOAT},
        {GL_SRGB8, PixelFormatNormalizeOptionBit::None, GL_SRGB8, GL_RGB, GL_UNSIGNED_BYTE},
        // Not widened even under the bit: no four-channel shared-exponent sibling exists.
        {GL_RGB9_E5, widen, GL_RGB9_E5, GL_RGB, GL_UNSIGNED_INT_5_9_9_9_REV},
        // Four-channel formats are unaffected by the bit; RGBA8_SNORM keeps its own fallback.
        {GL_RGBA8_SNORM, widen, GL_RGBA8_SNORM, GL_RGBA, GL_BYTE},
        {GL_RGBA8_SNORM, widen | PixelFormatNormalizeOptionBit::NoRGBA8Snorm, GL_RGBA16F, GL_RGBA, GL_FLOAT},
    };

    for (const auto& testCase : cases) {
        GLenum internalFormat = 0;
        GLenum format = 0;
        GLenum type = 0;
        NormalizePixelFormat(testCase.requested, testCase.options, &internalFormat, &format, &type);
        EXPECT_EQ(internalFormat, testCase.internalFormat) << "requested 0x" << std::hex << testCase.requested;
        EXPECT_EQ(format, testCase.format) << "requested 0x" << std::hex << testCase.requested;
        EXPECT_EQ(type, testCase.type) << "requested 0x" << std::hex << testCase.requested;
    }
}

TEST_F(TextureTest, WidenedRenderTargetUploadExpandsThreeChannelDataWithOpaqueAlpha) {
    using MobileGL::MG_Backend::DirectGLES::TextureImpl::GetWidenableClientComponentCount;
    using MobileGL::MG_Backend::DirectGLES::TextureImpl::PrepareChannelWidenedUpload;

    // Only the three-channel formats that can be widened report a source component count; the
    // repack is what keeps the driver from walking three texels' worth of data per four-texel row.
    for (const TextureInternalFormat format :
         {TextureInternalFormat::RGB8Snorm, TextureInternalFormat::RGB16F, TextureInternalFormat::RGB32F,
          TextureInternalFormat::RGB16Snorm, TextureInternalFormat::RGB16, TextureInternalFormat::SRGB8,
          TextureInternalFormat::RGB8UI, TextureInternalFormat::RGB32I}) {
        EXPECT_EQ(GetWidenableClientComponentCount(format), 3u)
            << MG_Util::ConvertTextureInternalFormatToString(format);
    }
    EXPECT_EQ(GetWidenableClientComponentCount(TextureInternalFormat::RGBA8), 0u);
    EXPECT_EQ(GetWidenableClientComponentCount(TextureInternalFormat::RGBA8Snorm), 0u);
    EXPECT_EQ(GetWidenableClientComponentCount(TextureInternalFormat::RGB9E5), 0u);

    const IntVec3 texelSize(2, 1, 1);

    // GL_RGB8_SNORM -> GL_RGBA16F: PrepareNormFloatFallbackUpload has already turned the Int8
    // shadow into floats, so what arrives here is three floats per texel.
    {
        const Float source[] = {0.25f, -0.5f, 0.75f, -1.0f, 0.0f, 1.0f};
        Vector<Uint8> widened;
        const auto* result = static_cast<const Float*>(PrepareChannelWidenedUpload(
            3, texelSize, source, sizeof(source), GL_FLOAT, widened));
        ASSERT_NE(result, static_cast<const void*>(source));
        ASSERT_EQ(widened.size(), 8 * sizeof(Float));
        const Float expected[] = {0.25f, -0.5f, 0.75f, 1.0f, -1.0f, 0.0f, 1.0f, 1.0f};
        for (SizeT i = 0; i < 8; ++i) {
            EXPECT_FLOAT_EQ(result[i], expected[i]) << "component " << i;
        }
    }

    // GL_RGB16F -> GL_RGBA16F uploads halves untouched, so the synthetic alpha is the half
    // encoding of 1.0 rather than a saturated field.
    {
        const Uint16 source[] = {0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006};
        Vector<Uint8> widened;
        const auto* result = static_cast<const Uint16*>(PrepareChannelWidenedUpload(
            3, texelSize, source, sizeof(source), GL_HALF_FLOAT, widened));
        ASSERT_NE(result, static_cast<const void*>(source));
        const Uint16 expected[] = {0x0001, 0x0002, 0x0003, 0x3C00, 0x0004, 0x0005, 0x0006, 0x3C00};
        for (SizeT i = 0; i < 8; ++i) {
            EXPECT_EQ(result[i], expected[i]) << "component " << i;
        }
    }

    // GL_SRGB8 -> GL_SRGB8_ALPHA8: fixed-point one is the saturated field.
    {
        const Uint8 source[] = {1, 2, 3, 4, 5, 6};
        Vector<Uint8> widened;
        const auto* result = static_cast<const Uint8*>(PrepareChannelWidenedUpload(
            3, texelSize, source, sizeof(source), GL_UNSIGNED_BYTE, widened));
        const Uint8 expected[] = {1, 2, 3, 0xFF, 4, 5, 6, 0xFF};
        ASSERT_NE(result, static_cast<const void*>(source));
        EXPECT_EQ(std::memcmp(result, expected, sizeof(expected)), 0);
    }

    // GL_RGB16_SNORM -> GL_RGBA16_SNORM keeps GL_SHORT, whose 1.0 is the positive maximum.
    {
        const Int16 source[] = {-1, 2, -3, 4, -5, 6};
        Vector<Uint8> widened;
        const auto* result = static_cast<const Int16*>(PrepareChannelWidenedUpload(
            3, texelSize, source, sizeof(source), GL_SHORT, widened));
        const Int16 expected[] = {-1, 2, -3, 0x7FFF, 4, -5, 6, 0x7FFF};
        ASSERT_NE(result, static_cast<const void*>(source));
        EXPECT_EQ(std::memcmp(result, expected, sizeof(expected)), 0);
    }

    // An integer format's added channel carries the integer one, not a saturated field.
    {
        const Uint32 source[] = {10, 20, 30, 40, 50, 60};
        Vector<Uint8> widened;
        const auto* result = static_cast<const Uint32*>(PrepareChannelWidenedUpload(
            3, texelSize, source, sizeof(source), GL_UNSIGNED_INT, widened, /*integerData=*/true));
        const Uint32 expected[] = {10, 20, 30, 1, 40, 50, 60, 1};
        ASSERT_NE(result, static_cast<const void*>(source));
        EXPECT_EQ(std::memcmp(result, expected, sizeof(expected)), 0);
    }

    // GL_RGB8I -> GL_RGBA8I uploads as GL_BYTE, the very type GL_RGB8_SNORM uses, so the type
    // alone cannot decide the added channel's value: the integer format's one is 1, the
    // signed-normalized format's is 0x7F. Getting this wrong is invisible through sampling and
    // glGetTexImage (both answer the alpha with the format's implied one) but escapes through a
    // blit or glCopyTexSubImage out of the widened attachment.
    {
        const Int8 source[] = {-1, 2, -3, 4, -5, 6};
        Vector<Uint8> widened;
        const auto* asInteger = static_cast<const Int8*>(PrepareChannelWidenedUpload(
            3, texelSize, source, sizeof(source), GL_BYTE, widened, /*integerData=*/true));
        const Int8 expectedInteger[] = {-1, 2, -3, 1, 4, -5, 6, 1};
        ASSERT_NE(asInteger, static_cast<const void*>(source));
        EXPECT_EQ(std::memcmp(asInteger, expectedInteger, sizeof(expectedInteger)), 0);

        Vector<Uint8> widenedNorm;
        const auto* asNormalized = static_cast<const Int8*>(PrepareChannelWidenedUpload(
            3, texelSize, source, sizeof(source), GL_BYTE, widenedNorm, /*integerData=*/false));
        const Int8 expectedNormalized[] = {-1, 2, -3, 0x7F, 4, -5, 6, 0x7F};
        EXPECT_EQ(std::memcmp(asNormalized, expectedNormalized, sizeof(expectedNormalized)), 0);
    }

    // Which class a widenable format belongs to.
    for (const TextureInternalFormat format :
         {TextureInternalFormat::RGB8I, TextureInternalFormat::RGB8UI, TextureInternalFormat::RGB16I,
          TextureInternalFormat::RGB16UI, TextureInternalFormat::RGB32I, TextureInternalFormat::RGB32UI}) {
        EXPECT_TRUE(MobileGL::MG_Backend::DirectGLES::TextureImpl::IsIntegerWidenableFormat(format))
            << MG_Util::ConvertTextureInternalFormatToString(format);
    }
    for (const TextureInternalFormat format :
         {TextureInternalFormat::RGB8Snorm, TextureInternalFormat::RGB16Snorm, TextureInternalFormat::RGB16,
          TextureInternalFormat::RGB16F, TextureInternalFormat::RGB32F, TextureInternalFormat::SRGB8}) {
        EXPECT_FALSE(MobileGL::MG_Backend::DirectGLES::TextureImpl::IsIntegerWidenableFormat(format))
            << MG_Util::ConvertTextureInternalFormatToString(format);
    }

    // The destination is sized from the level, never from the source. The driver reads a full
    // width*height*4 components for the transfer it was handed, so a short source must still
    // leave a full buffer behind - sizing it from the source would hand the driver a buffer it
    // runs off the end of.
    {
        const Float shortSource[] = {0.5f, 0.25f, 0.125f};
        Vector<Uint8> widened;
        const auto* result = static_cast<const Float*>(PrepareChannelWidenedUpload(
            3, IntVec3(2, 2, 1), shortSource, sizeof(shortSource), GL_FLOAT, widened));
        ASSERT_NE(result, static_cast<const void*>(shortSource));
        ASSERT_EQ(widened.size(), 4 * 4 * sizeof(Float));
        const Float expected[] = {0.5f, 0.25f, 0.125f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                                  0.0f, 0.0f,  0.0f,   1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
        for (SizeT i = 0; i < 16; ++i) {
            EXPECT_FLOAT_EQ(result[i], expected[i]) << "component " << i;
        }
    }

    // No widening in effect (or nothing to convert): the caller's pointer comes straight back, so
    // the sub-rect upload fast path still recognises an unconverted level.
    {
        const Float source[] = {1.0f, 2.0f, 3.0f, 4.0f};
        Vector<Uint8> widened;
        EXPECT_EQ(PrepareChannelWidenedUpload(4, texelSize, source, sizeof(source), GL_FLOAT, widened),
                  static_cast<const void*>(source));
        EXPECT_EQ(PrepareChannelWidenedUpload(0, texelSize, source, sizeof(source), GL_FLOAT, widened),
                  static_cast<const void*>(source));
        EXPECT_EQ(PrepareChannelWidenedUpload(3, texelSize, nullptr, 0, GL_FLOAT, widened), nullptr);
    }
}

// ---------------------------------------------------------------------------------------------
// A GL entry point may return an error, but it may never throw through the C GL ABI: unwinding a
// C++ exception across it terminates the process. These cover the sites that used to do exactly
// that (KHR-GL30.api.coverage died on the first of them on both backends).
// ---------------------------------------------------------------------------------------------

namespace {
    struct CopyTexImage2DCall {
        Bool Called = false;
        GLenum Target = 0;
        GLint Level = 0;
        GLenum InternalFormat = 0;
        GLsizei Width = 0;
        GLsizei Height = 0;
    };

    CopyTexImage2DCall g_copyTexImage2DCall;

    void RecordCopyTexImage2D(GLenum target, GLint level, GLenum internalformat, GLint, GLint, GLsizei width,
                              GLsizei height, GLint) {
        g_copyTexImage2DCall = {true, target, level, internalformat, width, height};
    }

    // A colour read framebuffer of the requested sized format, bound to GL_READ_FRAMEBUFFER, which
    // is what glCopyTexImage2D takes its source base format from.
    void BindReadFramebufferWithColorFormat(GLenum sizedInternalFormat) {
        GLuint framebuffer = 0;
        GLuint texture = 0;
        MG_Impl::GLImpl::CreateFramebuffers(1, &framebuffer);
        MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &texture);
        MG_Impl::GLImpl::TextureStorage2D(texture, 1, sizedInternalFormat, 16, 16);
        MG_Impl::GLImpl::NamedFramebufferTexture(framebuffer, GL_COLOR_ATTACHMENT0, texture, 0);
        MG_Impl::GLImpl::BindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    }

    GLuint BindFreshMutableTexture2D() {
        GLuint texture = 0;
        MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &texture);
        MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
        return texture;
    }
} // namespace

TEST_F(TextureTest, CopyTexImage2DAcceptsEveryComponentSubsetOfTheReadBuffer) {
    const ScopedTextureBackendFunctionsOverride backendGuard;
    MG_Backend::gBackendFunctionsTable.GL.CopyTexImage2D = RecordCopyTexImage2D;

    BindReadFramebufferWithColorFormat(GL_RGBA8);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR) << "read framebuffer setup itself failed";

    // GL 4.6 sec. 8.6: internalformat may name a SUBSET of the read buffer's components. This is
    // exactly the list KHR-GL30.api.coverage walks against an rgba8888 colour buffer, and it is
    // also what an ordinary GL app does with glCopyTexImage2D(GL_RGB) from an RGBA8 framebuffer.
    for (const GLenum internalFormat : {GL_RED, GL_RG, GL_RGB, GL_RGBA}) {
        BindFreshMutableTexture2D();
        g_copyTexImage2DCall = {};

        MG_Impl::GLImpl::CopyTexImage2D(GL_TEXTURE_2D, 0, internalFormat, 0, 0, 1, 1, 0);

        EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR) << "internalformat " << internalFormat;
        EXPECT_TRUE(g_copyTexImage2DCall.Called) << "internalformat " << internalFormat;
        EXPECT_EQ(g_copyTexImage2DCall.InternalFormat, internalFormat);
        EXPECT_EQ(g_copyTexImage2DCall.Width, 1);
        EXPECT_EQ(g_copyTexImage2DCall.Height, 1);
    }
}

TEST_F(TextureTest, CopyTexImage2DRejectsAFormatTheReadBufferCannotSupply) {
    const ScopedTextureBackendFunctionsOverride backendGuard;
    MG_Backend::gBackendFunctionsTable.GL.CopyTexImage2D = RecordCopyTexImage2D;

    BindReadFramebufferWithColorFormat(GL_R8);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR) << "read framebuffer setup itself failed";

    BindFreshMutableTexture2D();
    g_copyTexImage2DCall = {};

    // The subset rule still has a wrong side: GL_RGBA asks for components a GL_R8 read buffer does
    // not have. That must be GL_INVALID_OPERATION and nothing else - not a throw, not silence.
    MG_Impl::GLImpl::CopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, 1, 1, 0);

    ExpectSingleGlError(GL_INVALID_OPERATION);
    EXPECT_FALSE(g_copyTexImage2DCall.Called) << "a rejected copy must not reach the backend";
}

TEST_F(TextureTest, CopyTexImage1DReportsUnsupportedInsteadOfTerminating) {
    // 1D textures have no upload path in this stack; the entry point used to throw unconditionally.
    MG_Impl::GLImpl::CopyTexImage1D(GL_TEXTURE_1D, 0, GL_RGBA, 0, 0, 1, 0);
    ExpectSingleGlError(GL_INVALID_OPERATION);
}

TEST_F(TextureTest, GetTexLevelParameterAnswersBufferStorageGeometry) {
    // TextureStorageType is {Mipmap, Buffer} and the level queries used to answer only out of a
    // mipmap chain, so every glGetTexLevelParameter* on a GL_TEXTURE_BUFFER texture reached a
    // THROW_UNIMPL_EXCEPTION default: label and killed the process. It now answers out of the
    // attached buffer range instead (GL 4.6 core 8.9): a buffer texture is one-dimensional, and
    // with no buffer attached it addresses no texels at all.
    GLuint texture = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_BUFFER, 1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_BUFFER, texture);
    MG_Impl::GLImpl::TexBuffer(GL_TEXTURE_BUFFER, GL_R8, 0);
    DrainPendingGlErrors();

    const std::pair<GLenum, GLint> expectations[] = {
        {GL_TEXTURE_WIDTH, 0}, {GL_TEXTURE_HEIGHT, 1}, {GL_TEXTURE_DEPTH, 1}};
    for (const auto& [pname, expected] : expectations) {
        GLint intParam = 0x20202020;
        MG_Impl::GLImpl::GetTexLevelParameteriv(GL_TEXTURE_BUFFER, 0, pname, &intParam);
        EXPECT_EQ(MG_Impl::GLImpl::GetError(), static_cast<GLenum>(GL_NO_ERROR));
        EXPECT_EQ(intParam, expected) << "pname " << pname;

        GLfloat floatParam = 12345.0f;
        MG_Impl::GLImpl::GetTexLevelParameterfv(GL_TEXTURE_BUFFER, 0, pname, &floatParam);
        EXPECT_EQ(MG_Impl::GLImpl::GetError(), static_cast<GLenum>(GL_NO_ERROR));
        EXPECT_EQ(floatParam, static_cast<GLfloat>(expected)) << "pname " << pname;
    }
}

// Immutable storage plus glCompressedTexSubImage2D is the modern way to upload a compressed
// texture, so glTexStorage2D has to commit its levels to a specific compressed internalformat
// exactly as glTexImage2D does. When it did not, the sub-image call found an uncompressed level
// and refused it, and glTexImage2D and glTexStorage2D disagreed about the same token.
TEST_F(TextureTest, TexStorage2DTagsEveryLevelForASpecificCompressedFormat) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
    MG_Impl::GLImpl::TexStorage2D(GL_TEXTURE_2D, 2, GL_COMPRESSED_RED_RGTC1, 8, 8);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    for (GLint level = 0; level < 2; ++level) {
        GLint compressed = GL_FALSE;
        MG_Impl::GLImpl::GetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_COMPRESSED, &compressed);
        EXPECT_EQ(compressed, GL_TRUE) << "level " << level;
        GLint internalFormat = 0;
        MG_Impl::GLImpl::GetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_INTERNAL_FORMAT, &internalFormat);
        EXPECT_EQ(internalFormat, static_cast<GLint>(GL_COMPRESSED_RED_RGTC1)) << "level " << level;
        GLint imageSize = 0;
        MG_Impl::GLImpl::GetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_COMPRESSED_IMAGE_SIZE, &imageSize);
        // 8x8 -> 2x2 blocks -> 32 bytes; 4x4 -> 1 block -> 8 bytes.
        EXPECT_EQ(imageSize, level == 0 ? 32 : 8) << "level " << level;
    }
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // ...and the sub-image call the whole arrangement exists for now reaches both levels.
    Uint8 blocks[kRgtc1Size8x8];
    for (Int i = 0; i < kRgtc1Size8x8; ++i) blocks[i] = static_cast<Uint8>(0x40 + i);
    MG_Impl::GLImpl::CompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 8, 8, GL_COMPRESSED_RED_RGTC1, kRgtc1Size8x8,
                                             blocks);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    Uint8 stored[kRgtc1Size8x8] = {};
    MG_Impl::GLImpl::GetCompressedTexImage(GL_TEXTURE_2D, 0, stored);
    EXPECT_EQ(std::memcmp(stored, blocks, sizeof(stored)), 0);

    MG_Impl::GLImpl::CompressedTexSubImage2D(GL_TEXTURE_2D, 1, 0, 0, 4, 4, GL_COMPRESSED_RED_RGTC1, 8, blocks);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// The negative control: a generic compressed token leaves glTexStorage2D's levels uncompressed,
// because for those the implementation's choice IS the answer and MobileGL chooses uncompressed.
TEST_F(TextureTest, TexStorage2DLeavesAGenericCompressedFormatUncompressed) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
    MG_Impl::GLImpl::TexStorage2D(GL_TEXTURE_2D, 1, GL_COMPRESSED_RED, 8, 8);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    GLint compressed = GL_TRUE;
    MG_Impl::GLImpl::GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_COMPRESSED, &compressed);
    EXPECT_EQ(compressed, GL_FALSE);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// ===================== glCopyImageSubData validation (KHR-GL43.copy_image) =====================
//
// Every case below is a mechanism the conformance group caught in the field, and each one is
// pinned here because the backend cannot: a wrongly ACCEPTED copy shows up only as wrong pixels
// on a device, and a wrongly REJECTED one shows up only as a conformance failure.

namespace {
    struct CopyImageSubDataCall {
        Bool Called = false;
        GLenum SrcTarget = GL_NONE;
        GLenum DstTarget = GL_NONE;
        GLint SrcZ = -1;
        GLint DstZ = -1;
        GLsizei Depth = -1;
        Bool SrcIsRenderbuffer = false;
        Bool DstIsRenderbuffer = false;
    } g_copyImageSubDataCall;

    void RecordCopyImageSubData(const MG_Backend::CopyImageEndpoint& src, GLenum srcTarget,
                                GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ,
                                const MG_Backend::CopyImageEndpoint& dst, GLenum dstTarget,
                                GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ, GLsizei srcWidth,
                                GLsizei srcHeight, GLsizei srcDepth) {
        (void)srcLevel;
        (void)srcX;
        (void)srcY;
        (void)dstLevel;
        (void)dstX;
        (void)dstY;
        (void)srcWidth;
        (void)srcHeight;
        g_copyImageSubDataCall = {true,   srcTarget, dstTarget,           srcZ,
                                  dstZ,   srcDepth,  src.IsRenderbuffer(), dst.IsRenderbuffer()};
    }

    // Two storage-backed 2D textures of the requested formats, so a copy between them is a legal
    // call in every respect except the one the test is about.
    void MakeCopyImagePair(GLenum srcFormat, GLenum dstFormat, GLuint& srcTexture, GLuint& dstTexture,
                           GLsizei levels = 1, GLsizei extent = 8) {
        MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &srcTexture);
        MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &dstTexture);
        MG_Impl::GLImpl::TextureStorage2D(srcTexture, levels, srcFormat, extent, extent);
        MG_Impl::GLImpl::TextureStorage2D(dstTexture, levels, dstFormat, extent, extent);
    }
} // namespace

// GL 4.6 core 18.3.2 compatibility is texel-block SIZE, not base internal format. RGB10_A2 and
// R11F_G11F_B10F are both 32-bit and their bases differ (RGBA vs RGB); the old exact-base-format
// predicate rejected the pair, which is what took down the whole cross-format half of the
// conformance matrix on both backends.
TEST_F(TextureTest, CopyImageSubDataAcceptsEqualTexelSizeAcrossDifferentBaseFormats) {
    const ScopedTextureBackendFunctionsOverride backendGuard;
    MG_Backend::gBackendFunctionsTable.GL.CopyImageSubData = RecordCopyImageSubData;
    g_copyImageSubDataCall = {};

    GLuint srcTexture = 0;
    GLuint dstTexture = 0;
    MakeCopyImagePair(GL_RGB10_A2, GL_R11F_G11F_B10F, srcTexture, dstTexture);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::CopyImageSubData(srcTexture, GL_TEXTURE_2D, 0, 0, 0, 0, dstTexture, GL_TEXTURE_2D, 0, 0, 0, 0,
                                      4, 4, 1);
    EXPECT_TRUE(g_copyImageSubDataCall.Called);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// The other half of the same rule: equal base format is not sufficient either. RGBA8 and RGBA32F
// are both RGBA and 32 vs 128 bits, so the copy is illegal.
TEST_F(TextureTest, CopyImageSubDataRejectsDifferentTexelSizesWithTheSameBaseFormat) {
    const ScopedTextureBackendFunctionsOverride backendGuard;
    MG_Backend::gBackendFunctionsTable.GL.CopyImageSubData = RecordCopyImageSubData;
    g_copyImageSubDataCall = {};

    GLuint srcTexture = 0;
    GLuint dstTexture = 0;
    MakeCopyImagePair(GL_RGBA8, GL_RGBA32F, srcTexture, dstTexture);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::CopyImageSubData(srcTexture, GL_TEXTURE_2D, 0, 0, 0, 0, dstTexture, GL_TEXTURE_2D, 0, 0, 0, 0,
                                      4, 4, 1);
    EXPECT_FALSE(g_copyImageSubDataCall.Called);
    ExpectSingleGlError(GL_INVALID_OPERATION);
}

// ...and the pairing that is legal purely because the sizes agree, across integer-ness too.
TEST_F(TextureTest, CopyImageSubDataAcceptsIntegerAndFloatOfTheSameTexelSize) {
    const ScopedTextureBackendFunctionsOverride backendGuard;
    MG_Backend::gBackendFunctionsTable.GL.CopyImageSubData = RecordCopyImageSubData;
    g_copyImageSubDataCall = {};

    GLuint srcTexture = 0;
    GLuint dstTexture = 0;
    MakeCopyImagePair(GL_RGBA32UI, GL_RGBA32F, srcTexture, dstTexture);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::CopyImageSubData(srcTexture, GL_TEXTURE_2D, 0, 0, 0, 0, dstTexture, GL_TEXTURE_2D, 0, 0, 0, 0,
                                      4, 4, 1);
    EXPECT_TRUE(g_copyImageSubDataCall.Called);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// 18.3.2 spells a name that is not an object INVALID_VALUE. The shared texture-object validator
// says INVALID_OPERATION, which is right for the entry points that reach an object through a
// BINDING - hence a rule local to this entry point rather than a change to the helper.
TEST_F(TextureTest, CopyImageSubDataNonExistentNameIsInvalidValue) {
    const ScopedTextureBackendFunctionsOverride backendGuard;
    MG_Backend::gBackendFunctionsTable.GL.CopyImageSubData = RecordCopyImageSubData;
    g_copyImageSubDataCall = {};

    MG_Impl::GLImpl::CopyImageSubData(4242, GL_TEXTURE_2D, 0, 0, 0, 0, 4243, GL_TEXTURE_2D, 0, 0, 0, 0, 1, 1, 1);
    EXPECT_FALSE(g_copyImageSubDataCall.Called);
    ExpectSingleGlError(GL_INVALID_VALUE);
}

// A target that disagrees with the object it names is INVALID_ENUM, not the INVALID_OPERATION the
// shared target-uniformity validator records for the upload paths.
TEST_F(TextureTest, CopyImageSubDataTargetNotMatchingTheObjectIsInvalidEnum) {
    const ScopedTextureBackendFunctionsOverride backendGuard;
    MG_Backend::gBackendFunctionsTable.GL.CopyImageSubData = RecordCopyImageSubData;
    g_copyImageSubDataCall = {};

    GLuint srcTexture = 0;
    GLuint dstTexture = 0;
    MakeCopyImagePair(GL_RGBA8, GL_RGBA8, srcTexture, dstTexture);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::CopyImageSubData(srcTexture, GL_TEXTURE_2D, 0, 0, 0, 0, dstTexture, GL_TEXTURE_2D_ARRAY, 0, 0,
                                      0, 0, 1, 1, 1);
    EXPECT_FALSE(g_copyImageSubDataCall.Called);
    ExpectSingleGlError(GL_INVALID_ENUM);
}

// The eleven whole-image targets only: a cube FACE converts to a target the frontend knows, so the
// generic target validator lets it through, but 18.3.2 does not accept it here.
TEST_F(TextureTest, CopyImageSubDataRejectsTargetsOutsideTheSpecList) {
    const ScopedTextureBackendFunctionsOverride backendGuard;
    MG_Backend::gBackendFunctionsTable.GL.CopyImageSubData = RecordCopyImageSubData;
    g_copyImageSubDataCall = {};

    GLuint srcTexture = 0;
    GLuint dstTexture = 0;
    MakeCopyImagePair(GL_RGBA8, GL_RGBA8, srcTexture, dstTexture);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::CopyImageSubData(srcTexture, GL_TEXTURE_CUBE_MAP_POSITIVE_X, 0, 0, 0, 0, dstTexture,
                                      GL_TEXTURE_2D, 0, 0, 0, 0, 1, 1, 1);
    EXPECT_FALSE(g_copyImageSubDataCall.Called);
    ExpectSingleGlError(GL_INVALID_ENUM);
}

// A level the image does not have is INVALID_VALUE; a single-level texture asked for level 1 used
// to reach the backend with whatever the storage layer answered for that level.
TEST_F(TextureTest, CopyImageSubDataRejectsLevelTheImageDoesNotHave) {
    const ScopedTextureBackendFunctionsOverride backendGuard;
    MG_Backend::gBackendFunctionsTable.GL.CopyImageSubData = RecordCopyImageSubData;
    g_copyImageSubDataCall = {};

    GLuint srcTexture = 0;
    GLuint dstTexture = 0;
    MakeCopyImagePair(GL_RGBA8, GL_RGBA8, srcTexture, dstTexture);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::CopyImageSubData(srcTexture, GL_TEXTURE_2D, 0, 0, 0, 0, dstTexture, GL_TEXTURE_2D, 1, 0, 0, 0,
                                      1, 1, 1);
    EXPECT_FALSE(g_copyImageSubDataCall.Called);
    ExpectSingleGlError(GL_INVALID_VALUE);
}

// Sample counts must match. A single-sample image reports zero, so this same comparison is also
// what refuses a copy between a multisample target and a non-multisample one.
TEST_F(TextureTest, CopyImageSubDataRejectsSampleCountMismatch) {
    const ScopedTextureBackendFunctionsOverride backendGuard;
    MG_Backend::gBackendFunctionsTable.GL.CopyImageSubData = RecordCopyImageSubData;
    g_copyImageSubDataCall = {};

    // Two DIFFERENT counts are the whole point, so the case needs a context that can actually
    // create multisample storage - which this unit-test binary, with no backend behind the
    // renderable-format and sample-count queries, may not be able to. The precondition is
    // checked on the state objects rather than assumed, so this can only ever skip or test the
    // real rule; it can never pass vacuously.
    GLint maxSamples = 1;
    MG_Impl::GLImpl::GetIntegerv(GL_MAX_SAMPLES, &maxSamples);
    GLuint srcTexture = 0;
    GLuint dstTexture = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D_MULTISAMPLE, 1, &srcTexture);
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D_MULTISAMPLE, 1, &dstTexture);
    MG_Impl::GLImpl::TextureStorage2DMultisample(srcTexture, 1, GL_RGBA8, 8, 8, GL_FALSE);
    MG_Impl::GLImpl::TextureStorage2DMultisample(dstTexture, std::max(maxSamples, 2), GL_RGBA8, 8, 8, GL_FALSE);
    DrainPendingGlErrors();

    const Int srcSamples = MG_State::pGLContext->GetTextureObject(srcTexture)->GetSamples();
    const Int dstSamples = MG_State::pGLContext->GetTextureObject(dstTexture)->GetSamples();
    if (srcSamples == dstSamples) {
        GTEST_SKIP() << "this context could not give the two textures different sample counts (both " << srcSamples
                     << "); nothing for the rule to reject";
    }

    MG_Impl::GLImpl::CopyImageSubData(srcTexture, GL_TEXTURE_2D_MULTISAMPLE, 0, 0, 0, 0, dstTexture,
                                      GL_TEXTURE_2D_MULTISAMPLE, 0, 0, 0, 0, 1, 1, 1);
    EXPECT_FALSE(g_copyImageSubDataCall.Called);
    ExpectSingleGlError(GL_INVALID_OPERATION);
}

// The layer range has to survive the frontend intact. Both backends used to drop it - DirectVulkan
// pinned baseArrayLayer/layerCount at 0/1 - so a 12-layer copy moved one layer and said nothing;
// this pins the frontend half of that contract.
TEST_F(TextureTest, CopyImageSubDataForwardsTheWholeLayerRangeToTheBackend) {
    const ScopedTextureBackendFunctionsOverride backendGuard;
    MG_Backend::gBackendFunctionsTable.GL.CopyImageSubData = RecordCopyImageSubData;
    g_copyImageSubDataCall = {};

    GLuint srcTexture = 0;
    GLuint dstTexture = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D_ARRAY, 1, &srcTexture);
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D_ARRAY, 1, &dstTexture);
    MG_Impl::GLImpl::TextureStorage3D(srcTexture, 1, GL_RGBA8, 8, 8, 12);
    MG_Impl::GLImpl::TextureStorage3D(dstTexture, 1, GL_RGBA8, 8, 8, 12);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::CopyImageSubData(srcTexture, GL_TEXTURE_2D_ARRAY, 0, 0, 0, 2, dstTexture, GL_TEXTURE_2D_ARRAY,
                                      0, 0, 0, 5, 4, 4, 7);
    EXPECT_TRUE(g_copyImageSubDataCall.Called);
    EXPECT_EQ(g_copyImageSubDataCall.SrcZ, 2);
    EXPECT_EQ(g_copyImageSubDataCall.DstZ, 5);
    EXPECT_EQ(g_copyImageSubDataCall.Depth, 7);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// The shape KHR-GL43.copy_image.invalid_object ends on once the invalid-name cases are answered
// correctly: two ordinary glTexImage2D textures, no storage object, one texel copied from the
// origin. Nothing about it is exotic, which is exactly why it is worth a case of its own - every
// rule added to this validator is a new way to reject it.
TEST_F(TextureTest, CopyImageSubDataAcceptsAPlainMutableTexImage2DPair) {
    const ScopedTextureBackendFunctionsOverride backendGuard;
    MG_Backend::gBackendFunctionsTable.GL.CopyImageSubData = RecordCopyImageSubData;
    g_copyImageSubDataCall = {};

    GLuint srcTexture = 0;
    GLuint dstTexture = 0;
    MG_Impl::GLImpl::GenTextures(1, &srcTexture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, srcTexture);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    MG_Impl::GLImpl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    MG_Impl::GLImpl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    MG_Impl::GLImpl::GenTextures(1, &dstTexture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, dstTexture);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    MG_Impl::GLImpl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    MG_Impl::GLImpl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::CopyImageSubData(srcTexture, GL_TEXTURE_2D, 0, 0, 0, 0, dstTexture, GL_TEXTURE_2D, 0, 0, 0, 0,
                                      1, 1, 1);
    EXPECT_TRUE(g_copyImageSubDataCall.Called);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // ...and again after the names have been through a delete/regenerate cycle, which is what the
    // conformance case does between its sub-cases: it deletes an object to make it invalid, then
    // builds the next pair from names the allocator hands straight back.
    MG_Impl::GLImpl::DeleteTextures(1, &srcTexture);
    MG_Impl::GLImpl::DeleteTextures(1, &dstTexture);
    DrainPendingGlErrors();
    g_copyImageSubDataCall = {};

    GLuint reusedSrc = 0;
    GLuint reusedDst = 0;
    MG_Impl::GLImpl::GenTextures(1, &reusedSrc);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, reusedSrc);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    MG_Impl::GLImpl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    MG_Impl::GLImpl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    MG_Impl::GLImpl::GenTextures(1, &reusedDst);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, reusedDst);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    MG_Impl::GLImpl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    MG_Impl::GLImpl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::CopyImageSubData(reusedSrc, GL_TEXTURE_2D, 0, 0, 0, 0, reusedDst, GL_TEXTURE_2D, 0, 0, 0, 0, 1,
                                      1, 1);
    EXPECT_TRUE(g_copyImageSubDataCall.Called);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// A rectangle target reaches the backend as itself. The translation to the GL_TEXTURE_2D the ES
// driver actually stores it in belongs to DirectGLES, not here - and putting it here would break
// DirectVulkan, which needs the real target to tell an array copy from a flat one.
TEST_F(TextureTest, CopyImageSubDataPassesTheRectangleTargetThroughUntranslated) {
    const ScopedTextureBackendFunctionsOverride backendGuard;
    MG_Backend::gBackendFunctionsTable.GL.CopyImageSubData = RecordCopyImageSubData;
    g_copyImageSubDataCall = {};

    GLuint srcTexture = 0;
    GLuint dstTexture = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_RECTANGLE, 1, &srcTexture);
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_RECTANGLE, 1, &dstTexture);
    MG_Impl::GLImpl::TextureStorage2D(srcTexture, 1, GL_RGBA8, 8, 8);
    MG_Impl::GLImpl::TextureStorage2D(dstTexture, 1, GL_RGBA8, 8, 8);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::CopyImageSubData(srcTexture, GL_TEXTURE_RECTANGLE, 0, 0, 0, 0, dstTexture, GL_TEXTURE_RECTANGLE,
                                      0, 0, 0, 0, 4, 4, 1);
    EXPECT_TRUE(g_copyImageSubDataCall.Called);
    EXPECT_EQ(g_copyImageSubDataCall.SrcTarget, static_cast<GLenum>(GL_TEXTURE_RECTANGLE));
    EXPECT_EQ(g_copyImageSubDataCall.DstTarget, static_cast<GLenum>(GL_TEXTURE_RECTANGLE));
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// GL 4.6 core 18.3.2 accepts GL_RENDERBUFFER as an endpoint target, and a renderbuffer name lives
// in its own namespace. Resolving BOTH names through the texture namespace answered a null object
// for every renderbuffer endpoint, so all 74 conformance cases that name one - the whole
// texture<->renderbuffer half of KHR-GL43.copy_image, plus its smoke test - reported
// GL_INVALID_VALUE. The endpoint is a sum type now; the target picks the namespace.
TEST_F(TextureTest, CopyImageSubDataResolvesARenderbufferEndpointInTheRenderbufferNamespace) {
    const ScopedTextureBackendFunctionsOverride backendGuard;
    MG_Backend::gBackendFunctionsTable.GL.CopyImageSubData = RecordCopyImageSubData;
    g_copyImageSubDataCall = {};

    GLuint texture = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &texture);
    MG_Impl::GLImpl::TextureStorage2D(texture, 1, GL_RGBA8, 8, 8);
    GLuint renderbuffer = 0;
    MG_Impl::GLImpl::CreateRenderbuffers(1, &renderbuffer);
    MG_Impl::GLImpl::NamedRenderbufferStorage(renderbuffer, GL_RGBA8, 8, 8);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::CopyImageSubData(texture, GL_TEXTURE_2D, 0, 0, 0, 0, renderbuffer, GL_RENDERBUFFER, 0, 0, 0, 0,
                                      4, 4, 1);
    EXPECT_TRUE(g_copyImageSubDataCall.Called);
    EXPECT_FALSE(g_copyImageSubDataCall.SrcIsRenderbuffer);
    EXPECT_TRUE(g_copyImageSubDataCall.DstIsRenderbuffer);
    EXPECT_EQ(g_copyImageSubDataCall.DstTarget, static_cast<GLenum>(GL_RENDERBUFFER));
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // ...and back the other way, which is the second half of the conformance case's two-copy
    // shape (texture -> renderbuffer -> texture).
    g_copyImageSubDataCall = {};
    MG_Impl::GLImpl::CopyImageSubData(renderbuffer, GL_RENDERBUFFER, 0, 0, 0, 0, texture, GL_TEXTURE_2D, 0, 0, 0, 0,
                                      4, 4, 1);
    EXPECT_TRUE(g_copyImageSubDataCall.Called);
    EXPECT_TRUE(g_copyImageSubDataCall.SrcIsRenderbuffer);
    EXPECT_FALSE(g_copyImageSubDataCall.DstIsRenderbuffer);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// Renderbuffer to renderbuffer, the shape neither endpoint could take before, plus the negative
// that pins which table was consulted: with GL_RENDERBUFFER named, a number that is not a live
// RENDERBUFFER is INVALID_VALUE - the texture table is never asked.
TEST_F(TextureTest, CopyImageSubDataKeepsTheTwoNameNamespacesApart) {
    const ScopedTextureBackendFunctionsOverride backendGuard;
    MG_Backend::gBackendFunctionsTable.GL.CopyImageSubData = RecordCopyImageSubData;
    g_copyImageSubDataCall = {};

    GLuint srcRenderbuffer = 0;
    GLuint dstRenderbuffer = 0;
    MG_Impl::GLImpl::CreateRenderbuffers(1, &srcRenderbuffer);
    MG_Impl::GLImpl::CreateRenderbuffers(1, &dstRenderbuffer);
    MG_Impl::GLImpl::NamedRenderbufferStorage(srcRenderbuffer, GL_RGBA8, 8, 8);
    MG_Impl::GLImpl::NamedRenderbufferStorage(dstRenderbuffer, GL_RGBA8, 8, 8);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::CopyImageSubData(srcRenderbuffer, GL_RENDERBUFFER, 0, 0, 0, 0, dstRenderbuffer,
                                      GL_RENDERBUFFER, 0, 0, 0, 0, 4, 4, 1);
    EXPECT_TRUE(g_copyImageSubDataCall.Called);
    EXPECT_TRUE(g_copyImageSubDataCall.SrcIsRenderbuffer);
    EXPECT_TRUE(g_copyImageSubDataCall.DstIsRenderbuffer);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    g_copyImageSubDataCall = {};
    MG_Impl::GLImpl::CopyImageSubData(srcRenderbuffer, GL_RENDERBUFFER, 0, 0, 0, 0, 4243, GL_RENDERBUFFER, 0, 0, 0,
                                      0, 4, 4, 1);
    EXPECT_FALSE(g_copyImageSubDataCall.Called);
    ExpectSingleGlError(GL_INVALID_VALUE);
}

// A renderbuffer has exactly one image, so any level above zero is the same INVALID_VALUE a
// texture gets for a level it does not have - and an unallocated one is an incomplete image,
// which 18.3.2 spells INVALID_OPERATION.
TEST_F(TextureTest, CopyImageSubDataChecksARenderbufferLevelAndStorage) {
    const ScopedTextureBackendFunctionsOverride backendGuard;
    MG_Backend::gBackendFunctionsTable.GL.CopyImageSubData = RecordCopyImageSubData;
    g_copyImageSubDataCall = {};

    GLuint texture = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &texture);
    MG_Impl::GLImpl::TextureStorage2D(texture, 1, GL_RGBA8, 8, 8);
    GLuint renderbuffer = 0;
    MG_Impl::GLImpl::CreateRenderbuffers(1, &renderbuffer);
    MG_Impl::GLImpl::NamedRenderbufferStorage(renderbuffer, GL_RGBA8, 8, 8);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::CopyImageSubData(texture, GL_TEXTURE_2D, 0, 0, 0, 0, renderbuffer, GL_RENDERBUFFER, 1, 0, 0, 0,
                                      4, 4, 1);
    EXPECT_FALSE(g_copyImageSubDataCall.Called);
    ExpectSingleGlError(GL_INVALID_VALUE);

    g_copyImageSubDataCall = {};
    GLuint emptyRenderbuffer = 0;
    MG_Impl::GLImpl::CreateRenderbuffers(1, &emptyRenderbuffer);
    DrainPendingGlErrors();

    MG_Impl::GLImpl::CopyImageSubData(texture, GL_TEXTURE_2D, 0, 0, 0, 0, emptyRenderbuffer, GL_RENDERBUFFER, 0, 0,
                                      0, 0, 4, 4, 1);
    EXPECT_FALSE(g_copyImageSubDataCall.Called);
    ExpectSingleGlError(GL_INVALID_OPERATION);
}

// GL 4.6 core 18.3.2 requires INVALID_VALUE when the region exceeds either image's boundaries, and
// this validator had no bounds check whatsoever: the one call shaped like one,
// ValidateCopyImageBlockAlignment, returns true on its first line for every UNCOMPRESSED format.
// Texture endpoints only looked covered because the ES driver raised its own error - which
// DirectGLES logs and swallows, so the application saw GL_NO_ERROR and a destination that never
// changed (KHR-GL43.copy_image.exceeding_boundaries).
TEST_F(TextureTest, CopyImageSubDataRejectsARegionThatLeavesTheImage) {
    const ScopedTextureBackendFunctionsOverride backendGuard;
    MG_Backend::gBackendFunctionsTable.GL.CopyImageSubData = RecordCopyImageSubData;
    g_copyImageSubDataCall = {};

    GLuint srcTexture = 0;
    GLuint dstTexture = 0;
    MakeCopyImagePair(GL_RGBA8, GL_RGBA8, srcTexture, dstTexture);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // The region that exactly reaches the far edge is the boundary this must NOT reject - a
    // validator that answered INVALID_VALUE to every non-origin region would satisfy the negatives
    // below and break every legal partial copy.
    MG_Impl::GLImpl::CopyImageSubData(srcTexture, GL_TEXTURE_2D, 0, 4, 4, 0, dstTexture, GL_TEXTURE_2D, 0, 4, 4, 0,
                                      4, 4, 1);
    EXPECT_TRUE(g_copyImageSubDataCall.Called);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // One texel past it on x, on y, and on the destination side.
    g_copyImageSubDataCall = {};
    MG_Impl::GLImpl::CopyImageSubData(srcTexture, GL_TEXTURE_2D, 0, 5, 4, 0, dstTexture, GL_TEXTURE_2D, 0, 0, 0, 0,
                                      4, 4, 1);
    EXPECT_FALSE(g_copyImageSubDataCall.Called);
    ExpectSingleGlError(GL_INVALID_VALUE);

    g_copyImageSubDataCall = {};
    MG_Impl::GLImpl::CopyImageSubData(srcTexture, GL_TEXTURE_2D, 0, 4, 5, 0, dstTexture, GL_TEXTURE_2D, 0, 0, 0, 0,
                                      4, 4, 1);
    EXPECT_FALSE(g_copyImageSubDataCall.Called);
    ExpectSingleGlError(GL_INVALID_VALUE);

    g_copyImageSubDataCall = {};
    MG_Impl::GLImpl::CopyImageSubData(srcTexture, GL_TEXTURE_2D, 0, 0, 0, 0, dstTexture, GL_TEXTURE_2D, 0, 5, 5, 0,
                                      4, 4, 1);
    EXPECT_FALSE(g_copyImageSubDataCall.Called);
    ExpectSingleGlError(GL_INVALID_VALUE);

    // A negative origin is out of bounds on the other side of the same rule.
    g_copyImageSubDataCall = {};
    MG_Impl::GLImpl::CopyImageSubData(srcTexture, GL_TEXTURE_2D, 0, -1, 0, 0, dstTexture, GL_TEXTURE_2D, 0, 0, 0, 0,
                                      4, 4, 1);
    EXPECT_FALSE(g_copyImageSubDataCall.Called);
    ExpectSingleGlError(GL_INVALID_VALUE);
}

// The endpoint the missing bounds check actually cost: a renderbuffer never reaches the ES
// driver's texture-shaped checks either, so a 4x4 region at y = 14 of a 16x16 renderbuffer - the
// exact sub-case KHR-GL43.copy_image.exceeding_boundaries starts with, GL_RENDERBUFFER being first
// in its target list - was accepted outright.
TEST_F(TextureTest, CopyImageSubDataBoundsARenderbufferRegion) {
    const ScopedTextureBackendFunctionsOverride backendGuard;
    MG_Backend::gBackendFunctionsTable.GL.CopyImageSubData = RecordCopyImageSubData;
    g_copyImageSubDataCall = {};

    GLuint texture = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &texture);
    MG_Impl::GLImpl::TextureStorage2D(texture, 1, GL_RGBA8, 16, 16);
    GLuint renderbuffer = 0;
    MG_Impl::GLImpl::CreateRenderbuffers(1, &renderbuffer);
    MG_Impl::GLImpl::NamedRenderbufferStorage(renderbuffer, GL_RGBA8, 16, 16);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::CopyImageSubData(renderbuffer, GL_RENDERBUFFER, 0, 0, 12, 0, texture, GL_TEXTURE_2D, 0, 0, 0, 0,
                                      4, 4, 1);
    EXPECT_TRUE(g_copyImageSubDataCall.Called);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    g_copyImageSubDataCall = {};
    MG_Impl::GLImpl::CopyImageSubData(renderbuffer, GL_RENDERBUFFER, 0, 0, 14, 0, texture, GL_TEXTURE_2D, 0, 0, 0, 0,
                                      4, 4, 1);
    EXPECT_FALSE(g_copyImageSubDataCall.Called);
    ExpectSingleGlError(GL_INVALID_VALUE);

    // ...and as the destination, where the same renderbuffer has the same one image.
    g_copyImageSubDataCall = {};
    MG_Impl::GLImpl::CopyImageSubData(texture, GL_TEXTURE_2D, 0, 0, 0, 0, renderbuffer, GL_RENDERBUFFER, 0, 14, 0, 0,
                                      4, 4, 1);
    EXPECT_FALSE(g_copyImageSubDataCall.Called);
    ExpectSingleGlError(GL_INVALID_VALUE);

    // A renderbuffer has exactly one slice, so any z at all is out of range.
    g_copyImageSubDataCall = {};
    MG_Impl::GLImpl::CopyImageSubData(renderbuffer, GL_RENDERBUFFER, 0, 0, 0, 1, texture, GL_TEXTURE_2D, 0, 0, 0, 0,
                                      4, 4, 1);
    EXPECT_FALSE(g_copyImageSubDataCall.Called);
    ExpectSingleGlError(GL_INVALID_VALUE);
}

// The z axis was structurally unbounded - srcZ/dstZ did not even reach the validator - so a layer
// range running off the end of an array reached the backend as an out-of-range image subresource.
TEST_F(TextureTest, CopyImageSubDataBoundsTheLayerRangeOfAnArray) {
    const ScopedTextureBackendFunctionsOverride backendGuard;
    MG_Backend::gBackendFunctionsTable.GL.CopyImageSubData = RecordCopyImageSubData;
    g_copyImageSubDataCall = {};

    GLuint srcTexture = 0;
    GLuint dstTexture = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D_ARRAY, 1, &srcTexture);
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D_ARRAY, 1, &dstTexture);
    MG_Impl::GLImpl::TextureStorage3D(srcTexture, 1, GL_RGBA8, 8, 8, 12);
    MG_Impl::GLImpl::TextureStorage3D(dstTexture, 1, GL_RGBA8, 8, 8, 12);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // Layers 5..11 of a 12-layer array: the last one the range may reach.
    MG_Impl::GLImpl::CopyImageSubData(srcTexture, GL_TEXTURE_2D_ARRAY, 0, 0, 0, 5, dstTexture, GL_TEXTURE_2D_ARRAY,
                                      0, 0, 0, 5, 4, 4, 7);
    EXPECT_TRUE(g_copyImageSubDataCall.Called);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    g_copyImageSubDataCall = {};
    MG_Impl::GLImpl::CopyImageSubData(srcTexture, GL_TEXTURE_2D_ARRAY, 0, 0, 0, 6, dstTexture, GL_TEXTURE_2D_ARRAY,
                                      0, 0, 0, 0, 4, 4, 7);
    EXPECT_FALSE(g_copyImageSubDataCall.Called);
    ExpectSingleGlError(GL_INVALID_VALUE);

    g_copyImageSubDataCall = {};
    MG_Impl::GLImpl::CopyImageSubData(srcTexture, GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, dstTexture, GL_TEXTURE_2D_ARRAY,
                                      0, 0, 0, 6, 4, 4, 7);
    EXPECT_FALSE(g_copyImageSubDataCall.Called);
    ExpectSingleGlError(GL_INVALID_VALUE);
}

// The convention the bounds check has to get right, and the one that would silently reject legal
// copies if it did not: on a CUBE MAP the z axis selects among the six faces, which this frontend
// keeps as six separate one-slice upload targets - so the level's own extent reports depth 1 and a
// bound taken from it would refuse every whole-cube copy.
TEST_F(TextureTest, CopyImageSubDataCountsCubeMapFacesOnTheZAxis) {
    const ScopedTextureBackendFunctionsOverride backendGuard;
    MG_Backend::gBackendFunctionsTable.GL.CopyImageSubData = RecordCopyImageSubData;
    g_copyImageSubDataCall = {};

    GLuint srcTexture = 0;
    GLuint dstTexture = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_CUBE_MAP, 1, &srcTexture);
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_CUBE_MAP, 1, &dstTexture);
    MG_Impl::GLImpl::TextureStorage2D(srcTexture, 1, GL_RGBA8, 8, 8);
    MG_Impl::GLImpl::TextureStorage2D(dstTexture, 1, GL_RGBA8, 8, 8);
    DrainPendingGlErrors();

    const auto srcObject = MG_State::pGLContext->GetTextureObject(srcTexture);
    const auto dstObject = MG_State::pGLContext->GetTextureObject(dstTexture);
    ASSERT_NE(srcObject, nullptr);
    ASSERT_NE(dstObject, nullptr);
    if (!srcObject->IsComplete() || !dstObject->IsComplete()) {
        GTEST_SKIP() << "this context could not give the cube maps storage";
    }

    MG_Impl::GLImpl::CopyImageSubData(srcTexture, GL_TEXTURE_CUBE_MAP, 0, 0, 0, 0, dstTexture, GL_TEXTURE_CUBE_MAP,
                                      0, 0, 0, 0, 8, 8, 6);
    EXPECT_TRUE(g_copyImageSubDataCall.Called);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // A seventh face does not exist.
    g_copyImageSubDataCall = {};
    MG_Impl::GLImpl::CopyImageSubData(srcTexture, GL_TEXTURE_CUBE_MAP, 0, 0, 0, 1, dstTexture, GL_TEXTURE_CUBE_MAP,
                                      0, 0, 0, 0, 8, 8, 6);
    EXPECT_FALSE(g_copyImageSubDataCall.Called);
    ExpectSingleGlError(GL_INVALID_VALUE);
}

// The other axis convention, and it is NOT the one this frontend stores. GL 4.6 core 18.3.2
// treats every array texture as a stack of slices on Z and gives a 1D array an image HEIGHT OF
// ONE (which is exactly what the CTS asserts: it forces height = 1 for GL_TEXTURE_1D_ARRAY and
// lists the target as multilayer). MobileGL keeps a 1D array's layers on y internally - that is
// what glTexImage2D(GL_TEXTURE_1D_ARRAY, w, layers) writes - so this entry point has to convert,
// and measuring srcY against the LAYER count is what let an out-of-range srcY come back
// GL_NO_ERROR (KHR-GL43.copy_image.exceeding_boundaries, the src_test_case y variants).
TEST_F(TextureTest, CopyImageSubDataBoundsA1DArraysLayersOnTheZAxis) {
    const ScopedTextureBackendFunctionsOverride backendGuard;
    MG_Backend::gBackendFunctionsTable.GL.CopyImageSubData = RecordCopyImageSubData;
    g_copyImageSubDataCall = {};

    GLuint srcTexture = 0;
    GLuint dstTexture = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_1D_ARRAY, 1, &srcTexture);
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_1D_ARRAY, 1, &dstTexture);
    MG_Impl::GLImpl::TextureStorage2D(srcTexture, 1, GL_RGBA8, 16, 8);
    MG_Impl::GLImpl::TextureStorage2D(dstTexture, 1, GL_RGBA8, 16, 8);
    DrainPendingGlErrors();

    const auto srcObject = MG_State::pGLContext->GetTextureObject(srcTexture);
    const auto dstObject = MG_State::pGLContext->GetTextureObject(dstTexture);
    ASSERT_NE(srcObject, nullptr);
    ASSERT_NE(dstObject, nullptr);
    if (!srcObject->IsComplete() || !dstObject->IsComplete()) {
        GTEST_SKIP() << "this context could not give the 1D arrays storage";
    }

    // 16 wide, 8 layers. Five layers from layer 3 is legal, and it is spelled on z with a
    // height of 1 - the layer count rides on srcDepth.
    MG_Impl::GLImpl::CopyImageSubData(srcTexture, GL_TEXTURE_1D_ARRAY, 0, 0, 0, 3, dstTexture, GL_TEXTURE_1D_ARRAY,
                                      0, 0, 0, 3, 4, 1, 5);
    EXPECT_TRUE(g_copyImageSubDataCall.Called);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // One layer past the last one is out of bounds on z.
    g_copyImageSubDataCall = {};
    MG_Impl::GLImpl::CopyImageSubData(srcTexture, GL_TEXTURE_1D_ARRAY, 0, 0, 0, 4, dstTexture, GL_TEXTURE_1D_ARRAY,
                                      0, 0, 0, 0, 4, 1, 5);
    EXPECT_FALSE(g_copyImageSubDataCall.Called);
    ExpectSingleGlError(GL_INVALID_VALUE);

    // The image is one texel HIGH whatever its layer count is, so any srcY past 0 is out of
    // bounds - this is the KHR-GL43.copy_image case that used to be measured against the 8
    // layers and pass.
    g_copyImageSubDataCall = {};
    MG_Impl::GLImpl::CopyImageSubData(srcTexture, GL_TEXTURE_1D_ARRAY, 0, 0, 6, 0, dstTexture, GL_TEXTURE_1D_ARRAY,
                                      0, 0, 6, 0, 4, 1, 1);
    EXPECT_FALSE(g_copyImageSubDataCall.Called);
    ExpectSingleGlError(GL_INVALID_VALUE);

    // ... and a height of 1 at y = 0 is the only legal y extent.
    g_copyImageSubDataCall = {};
    MG_Impl::GLImpl::CopyImageSubData(srcTexture, GL_TEXTURE_1D_ARRAY, 0, 0, 0, 0, dstTexture, GL_TEXTURE_1D_ARRAY,
                                      0, 0, 0, 0, 4, 2, 1);
    EXPECT_FALSE(g_copyImageSubDataCall.Called);
    ExpectSingleGlError(GL_INVALID_VALUE);
}

// A 16-byte RGTC2 block and a 16-byte RGBA32UI texel are in the same size class, so GL 4.6 core
// 18.3.2 requires this copy to succeed. It did not for an ARRAY source: glTexImage3D recorded no
// specific-compressed-format tag, so the level was measured as the 2-byte RG8 storage RGTC2
// resolves to and the compatibility rule saw 2 against 16.
TEST_F(TextureTest, CopyImageSubDataSizesACompressedArrayLevelByItsBlock) {
    const ScopedTextureBackendFunctionsOverride backendGuard;
    MG_Backend::gBackendFunctionsTable.GL.CopyImageSubData = RecordCopyImageSubData;
    g_copyImageSubDataCall = {};

    GLuint compressedSource = 0;
    MG_Impl::GLImpl::GenTextures(1, &compressedSource);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D_ARRAY, compressedSource);
    MG_Impl::GLImpl::TexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_COMPRESSED_RG_RGTC2, 8, 8, 1, 0, GL_RG,
                                GL_UNSIGNED_BYTE, nullptr);
    MG_Impl::GLImpl::TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BASE_LEVEL, 0);
    MG_Impl::GLImpl::TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 0);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D_ARRAY, 0);

    GLuint uncompressedDestination = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D_ARRAY, 1, &uncompressedDestination);
    MG_Impl::GLImpl::TextureStorage3D(uncompressedDestination, 1, GL_RGBA32UI, 8, 8, 1);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::CopyImageSubData(compressedSource, GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, uncompressedDestination,
                                      GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, 8, 8, 1);
    EXPECT_TRUE(g_copyImageSubDataCall.Called);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// 18.3.2 requires INVALID_OPERATION when either object is an INCOMPLETE TEXTURE, and completeness
// is GL 4.6 core 8.17's - which includes the mip chain whenever the minification filter reads it.
// A mutable texture with level 0 alone still carries the default NEAREST_MIPMAP_LINEAR filter, so
// it is mipmap incomplete; the storage-only IsComplete() this used to ask called it complete and
// let the copy through, which is the whole of KHR-GL43.copy_image.incomplete_tex.
TEST_F(TextureTest, CopyImageSubDataRejectsAMipmapIncompleteTexture) {
    const ScopedTextureBackendFunctionsOverride backendGuard;
    MG_Backend::gBackendFunctionsTable.GL.CopyImageSubData = RecordCopyImageSubData;
    g_copyImageSubDataCall = {};

    GLuint incomplete = 0;
    MG_Impl::GLImpl::GenTextures(1, &incomplete);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, incomplete);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, 0);
    GLuint complete = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &complete);
    MG_Impl::GLImpl::TextureStorage2D(complete, 1, GL_RGBA8, 16, 16);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::CopyImageSubData(incomplete, GL_TEXTURE_2D, 0, 0, 0, 0, complete, GL_TEXTURE_2D, 0, 0, 0, 0, 4,
                                      4, 1);
    EXPECT_FALSE(g_copyImageSubDataCall.Called);
    ExpectSingleGlError(GL_INVALID_OPERATION);

    // The destination side is checked the same way.
    g_copyImageSubDataCall = {};
    MG_Impl::GLImpl::CopyImageSubData(complete, GL_TEXTURE_2D, 0, 0, 0, 0, incomplete, GL_TEXTURE_2D, 0, 0, 0, 0, 4,
                                      4, 1);
    EXPECT_FALSE(g_copyImageSubDataCall.Called);
    ExpectSingleGlError(GL_INVALID_OPERATION);

    // Capping TEXTURE_MAX_LEVEL at the one level that exists is what the conformance suite's
    // makeTextureComplete does, and it is enough to make the same object complete.
    g_copyImageSubDataCall = {};
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, incomplete);
    MG_Impl::GLImpl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    MG_Impl::GLImpl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, 0);
    DrainPendingGlErrors();

    MG_Impl::GLImpl::CopyImageSubData(incomplete, GL_TEXTURE_2D, 0, 0, 0, 0, complete, GL_TEXTURE_2D, 0, 0, 0, 0, 4,
                                      4, 1);
    EXPECT_TRUE(g_copyImageSubDataCall.Called);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// The targets that have no mip chain must not be dragged in: GL 4.6 core 8.17 makes q equal to
// level_base for them, so no filter can make them mipmap incomplete. A rectangle texture gets a
// non-mipmapping default filter from the object itself, so it would survive a predicate that
// trusted the sampler alone - it is here because the whole texture path is one branch and this is
// the cheap half of pinning it.
TEST_F(TextureTest, CopyImageSubDataDoesNotApplyMipmapCompletenessToRectangleTextures) {
    const ScopedTextureBackendFunctionsOverride backendGuard;
    MG_Backend::gBackendFunctionsTable.GL.CopyImageSubData = RecordCopyImageSubData;
    g_copyImageSubDataCall = {};

    GLuint srcRectangle = 0;
    GLuint dstRectangle = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_RECTANGLE, 1, &srcRectangle);
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_RECTANGLE, 1, &dstRectangle);
    MG_Impl::GLImpl::TextureStorage2D(srcRectangle, 1, GL_RGBA8, 8, 8);
    MG_Impl::GLImpl::TextureStorage2D(dstRectangle, 1, GL_RGBA8, 8, 8);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::CopyImageSubData(srcRectangle, GL_TEXTURE_RECTANGLE, 0, 0, 0, 0, dstRectangle,
                                      GL_TEXTURE_RECTANGLE, 0, 0, 0, 0, 4, 4, 1);
    EXPECT_TRUE(g_copyImageSubDataCall.Called);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// The multisample half, which is the one the target guard actually exists for: a multisample
// texture keeps the shared NEAREST_MIPMAP_LINEAR default in its own sampler state (only the
// rectangle constructor overrides it), so asking the mipmap predicate about it without the target
// guard would report every 8x8 multisample image incomplete and refuse a legal copy.
TEST_F(TextureTest, CopyImageSubDataDoesNotApplyMipmapCompletenessToMultisampleTextures) {
    const ScopedTextureBackendFunctionsOverride backendGuard;
    MG_Backend::gBackendFunctionsTable.GL.CopyImageSubData = RecordCopyImageSubData;
    g_copyImageSubDataCall = {};

    GLuint srcMultisample = 0;
    GLuint dstMultisample = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D_MULTISAMPLE, 1, &srcMultisample);
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D_MULTISAMPLE, 1, &dstMultisample);
    MG_Impl::GLImpl::TextureStorage2DMultisample(srcMultisample, 1, GL_RGBA8, 8, 8, GL_FALSE);
    MG_Impl::GLImpl::TextureStorage2DMultisample(dstMultisample, 1, GL_RGBA8, 8, 8, GL_FALSE);
    DrainPendingGlErrors();

    // This unit-test binary has no backend behind the renderable-format and sample-count queries,
    // so the storage may not have been created at all. Checked on the state objects rather than
    // assumed, so the case can only skip or test the real rule.
    const auto srcObject = MG_State::pGLContext->GetTextureObject(srcMultisample);
    const auto dstObject = MG_State::pGLContext->GetTextureObject(dstMultisample);
    ASSERT_NE(srcObject, nullptr);
    ASSERT_NE(dstObject, nullptr);
    if (!srcObject->IsComplete() || !dstObject->IsComplete()) {
        GTEST_SKIP() << "this context could not give the multisample textures storage";
    }

    MG_Impl::GLImpl::CopyImageSubData(srcMultisample, GL_TEXTURE_2D_MULTISAMPLE, 0, 0, 0, 0, dstMultisample,
                                      GL_TEXTURE_2D_MULTISAMPLE, 0, 0, 0, 0, 4, 4, 1);
    EXPECT_TRUE(g_copyImageSubDataCall.Called);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// GL 4.6 core 8.11 makes GL_IMAGE_FORMAT_COMPATIBILITY_TYPE readable through every
// GetTexParameter form. Three of MobileGL's four getters answered it and glGetTexParameterfv did
// not, so the float query raised GL_INVALID_ENUM and left the caller's float uninitialised
// (KHR-GL4x.shader_image_load_store.basic-api-texParam reads it with both iv and fv and compares
// them). Asserted across all four here, because an enum present in three of four parallel
// switches is the drift shape that comes back.
TEST_F(TextureTest, ImageFormatCompatibilityTypeAgreesAcrossEveryTexParameterGetter) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
    MG_Impl::GLImpl::TexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 4, 4);
    DrainPendingGlErrors();

    GLint integerValue = 0;
    MG_Impl::GLImpl::GetTexParameteriv(GL_TEXTURE_2D, GL_IMAGE_FORMAT_COMPATIBILITY_TYPE, &integerValue);
    EXPECT_EQ(integerValue, GL_IMAGE_FORMAT_COMPATIBILITY_BY_SIZE);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    GLfloat floatValue = 0.0f;
    MG_Impl::GLImpl::GetTexParameterfv(GL_TEXTURE_2D, GL_IMAGE_FORMAT_COMPATIBILITY_TYPE, &floatValue);
    EXPECT_FLOAT_EQ(floatValue, static_cast<GLfloat>(GL_IMAGE_FORMAT_COMPATIBILITY_BY_SIZE));
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    GLint signedValue = 0;
    MG_Impl::GLImpl::GetTexParameterIiv(GL_TEXTURE_2D, GL_IMAGE_FORMAT_COMPATIBILITY_TYPE, &signedValue);
    EXPECT_EQ(signedValue, GL_IMAGE_FORMAT_COMPATIBILITY_BY_SIZE);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    GLuint unsignedValue = 0;
    MG_Impl::GLImpl::GetTexParameterIuiv(GL_TEXTURE_2D, GL_IMAGE_FORMAT_COMPATIBILITY_TYPE, &unsignedValue);
    EXPECT_EQ(unsignedValue, static_cast<GLuint>(GL_IMAGE_FORMAT_COMPATIBILITY_BY_SIZE));
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, 0);
    MG_Impl::GLImpl::DeleteTextures(1, &texture);
    DrainPendingGlErrors();
}

// The image-format widening's transfer half. GL has forty image formats and GLSL ES core has
// thirteen, so an image-bindable GL_R8UI texture is stored as a GL_RGBA8UI and an image-bindable
// GL_RG32F as a GL_RGBA32F (TextureImpl::GetImageBindableStorageWidening). The driver is then told
// the transfer is four components wide, and one- or two-component client data has to be repacked to
// match - with the SAME values GL gives the channels a narrow format does not have, so that a
// later sample, imageLoad or glGetTexImage cannot tell the carrier from the real thing.
TEST_F(TextureTest, ImageWidenedUploadExpandsOneAndTwoChannelDataWithGLsMissingChannelValues) {
    using MobileGL::MG_Backend::DirectGLES::TextureImpl::PrepareChannelWidenedUpload;

    const IntVec3 texelSize(2, 1, 1);

    // GL_RG32F -> GL_RGBA32F. Blue is 0 and alpha 1.0, which is exactly what GL answers for the
    // two channels an rg32f image does not have.
    {
        const Float source[] = {0.25f, -0.5f, 1.5f, -2.5f};
        Vector<Uint8> widened;
        const auto* result = static_cast<const Float*>(
            PrepareChannelWidenedUpload(2, texelSize, source, sizeof(source), GL_FLOAT, widened, false));
        ASSERT_NE(result, static_cast<const void*>(source));
        ASSERT_EQ(widened.size(), 8 * sizeof(Float));
        const Float expected[] = {0.25f, -0.5f, 0.0f, 1.0f, 1.5f, -2.5f, 0.0f, 1.0f};
        for (SizeT i = 0; i < 8; ++i) {
            EXPECT_FLOAT_EQ(result[i], expected[i]) << "component " << i;
        }
    }

    // GL_R8UI -> GL_RGBA8UI. Three added channels, and the one in alpha is the INTEGER one: an
    // integer format's missing alpha reads back as 1, not as the saturated field a normalized
    // format's does, and GL_UNSIGNED_BYTE serves both classes so the type alone cannot decide.
    {
        const Uint8 source[] = {7, 8};
        Vector<Uint8> widened;
        const auto* result = static_cast<const Uint8*>(PrepareChannelWidenedUpload(
            1, texelSize, source, sizeof(source), GL_UNSIGNED_BYTE, widened, /*integerData=*/true));
        ASSERT_NE(result, static_cast<const void*>(source));
        const Uint8 expected[] = {7, 0, 0, 1, 8, 0, 0, 1};
        ASSERT_EQ(widened.size(), sizeof(expected));
        EXPECT_EQ(std::memcmp(result, expected, sizeof(expected)), 0);
    }

    // GL_R8 -> GL_RGBA8, the normalized twin of the case above: same transfer type, saturated one.
    {
        const Uint8 source[] = {7, 8};
        Vector<Uint8> widened;
        const auto* result = static_cast<const Uint8*>(PrepareChannelWidenedUpload(
            1, texelSize, source, sizeof(source), GL_UNSIGNED_BYTE, widened, /*integerData=*/false));
        const Uint8 expected[] = {7, 0, 0, 0xFF, 8, 0, 0, 0xFF};
        ASSERT_NE(result, static_cast<const void*>(source));
        EXPECT_EQ(std::memcmp(result, expected, sizeof(expected)), 0);
    }

    // GL_RG8_SNORM -> GL_RGBA8_SNORM keeps GL_BYTE, whose 1.0 is the positive maximum.
    {
        const Int8 source[] = {-1, 2, 3, -4};
        Vector<Uint8> widened;
        const auto* result = static_cast<const Int8*>(
            PrepareChannelWidenedUpload(2, texelSize, source, sizeof(source), GL_BYTE, widened, false));
        const Int8 expected[] = {-1, 2, 0, 0x7F, 3, -4, 0, 0x7F};
        ASSERT_NE(result, static_cast<const void*>(source));
        EXPECT_EQ(std::memcmp(result, expected, sizeof(expected)), 0);
    }

    // A four-component source is already the carrier's shape: nothing to repack, and the caller's
    // sub-rect upload fast path depends on the pointer coming back unchanged when that is so.
    {
        const Uint8 source[] = {1, 2, 3, 4, 5, 6, 7, 8};
        Vector<Uint8> widened;
        EXPECT_EQ(PrepareChannelWidenedUpload(4, texelSize, source, sizeof(source), GL_UNSIGNED_BYTE, widened,
                                              false),
                  static_cast<const void*>(source));
        EXPECT_TRUE(widened.empty());
    }
}

// The OTHER transfer shape the image widening needs, and the one a channel repack cannot serve:
// GL_RGB10_A2UI's shadow is ONE 32-bit word per texel, not four components of the GL_RGBA16UI
// carrier's own type. Repacking it as components would take sixteen bytes out of a four-byte texel
// and shear the level - which only a LOAD notices, because a store overwrites whatever the upload
// got wrong.
//
// GL_UNSIGNED_INT_2_10_10_10_REV puts the FIRST component in the LOW bits, which is the whole
// content of the word "REV" and the single thing this can get backwards, so every field here is a
// different value and the boundary codes (0, the 10-bit maximum, the 2-bit maximum) are pinned
// exactly rather than compared with a tolerance.
TEST_F(TextureTest, ImageWidenedUploadSplitsAPacked2101010RevShadowIntoFourChannelCodes) {
    using MobileGL::MG_Backend::DirectGLES::TextureImpl::PreparePackedIntWidenedUpload;

    const IntVec3 texelSize(3, 1, 1);
    // r=1, g=2, b=3, a=1 | r=1023, g=0, b=1023, a=3 | r=0, g=1023, b=0, a=0
    const Uint32 source[] = {
        1u | (2u << 10) | (3u << 20) | (1u << 30),
        1023u | (0u << 10) | (1023u << 20) | (3u << 30),
        0u | (1023u << 10) | (0u << 20) | (0u << 30),
    };
    Vector<Uint8> widened;
    const auto* result = static_cast<const Uint16*>(
        PreparePackedIntWidenedUpload(texelSize, source, sizeof(source), widened));
    ASSERT_NE(result, static_cast<const void*>(source));
    ASSERT_EQ(widened.size(), 12 * sizeof(Uint16));
    const Uint16 expected[] = {1, 2, 3, 1, 1023, 0, 1023, 3, 0, 1023, 0, 0};
    for (SizeT i = 0; i < 12; ++i) {
        EXPECT_EQ(result[i], expected[i]) << "component " << i;
    }

    // Sized from the LEVEL, never from the source: the driver reads a full width*height*4 shorts
    // for the transfer it was handed, so a short source still has to leave a full destination.
    {
        Vector<Uint8> shortWidened;
        const auto* shortResult = static_cast<const Uint16*>(
            PreparePackedIntWidenedUpload(texelSize, source, sizeof(Uint32), shortWidened));
        ASSERT_EQ(shortWidened.size(), 12 * sizeof(Uint16));
        for (SizeT i = 4; i < 12; ++i) {
            EXPECT_EQ(shortResult[i], 0u) << "component " << i << " past the source must be zero";
        }
    }

    // Nothing to split.
    {
        Vector<Uint8> empty;
        EXPECT_EQ(PreparePackedIntWidenedUpload(texelSize, nullptr, 0, empty), nullptr);
        EXPECT_TRUE(empty.empty());
    }
}

// ---------------------------------------------------------------------------------------------
// GL_TEXTURE_CUBE_MAP_ARRAY: the shape rules, the shared-exponent level query, and the
// three-dimensional bound-texture copy. All three were front-end gaps rather than backend ones -
// the DirectVulkan baseline failed the identical conformance bodies.
// ---------------------------------------------------------------------------------------------

// glTexStorage3D carried the two cube-array shape rules inline and glTexImage3D carried neither,
// which is exactly why esextcTextureCubeMapArrayTex3DValidation failed on its two glTexImage3D
// assertions and passed both glTexStorage3D ones. The predicate now lives in one validator that
// every level-defining entry point calls.
TEST_F(TextureTest, TexImage3DAppliesTheCubeMapArrayShapeRules) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, texture);
    DrainPendingGlErrors();

    // Non-square faces are GL_INVALID_VALUE.
    MG_Impl::GLImpl::TexImage3D(GL_TEXTURE_CUBE_MAP_ARRAY, 0, GL_RGBA8, 4, 8, 6, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                                nullptr);
    ExpectSingleGlError(GL_INVALID_VALUE);

    // A depth that is not a whole number of cubes is GL_INVALID_VALUE.
    MG_Impl::GLImpl::TexImage3D(GL_TEXTURE_CUBE_MAP_ARRAY, 0, GL_RGBA8, 4, 4, 5, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                                nullptr);
    ExpectSingleGlError(GL_INVALID_VALUE);

    // The legal shape still goes through untouched.
    MG_Impl::GLImpl::TexImage3D(GL_TEXTURE_CUBE_MAP_ARRAY, 0, GL_RGBA8, 4, 4, 12, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                                nullptr);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // And the rules are not applied to targets they do not belong to: a 2D array may be any
    // rectangle with any layer count.
    GLuint arrayTexture = 0;
    MG_Impl::GLImpl::GenTextures(1, &arrayTexture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D_ARRAY, arrayTexture);
    MG_Impl::GLImpl::TexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 4, 8, 5, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D_ARRAY, 0);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, 0);
}

TEST_F(TextureTest, TexStorage3DKeepsTheCubeMapArrayShapeRulesAfterTheyMovedIntoTheSharedValidator) {
    GLuint texture = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_CUBE_MAP_ARRAY, 1, &texture);
    DrainPendingGlErrors();

    MG_Impl::GLImpl::TextureStorage3D(texture, 1, GL_RGBA8, 4, 8, 6);
    ExpectSingleGlError(GL_INVALID_VALUE);

    GLuint second = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_CUBE_MAP_ARRAY, 1, &second);
    MG_Impl::GLImpl::TextureStorage3D(second, 1, GL_RGBA8, 4, 4, 5);
    ExpectSingleGlError(GL_INVALID_VALUE);

    GLuint third = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_CUBE_MAP_ARRAY, 1, &third);
    MG_Impl::GLImpl::TextureStorage3D(third, 1, GL_RGBA8, 4, 4, 6);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// GL_TEXTURE_SHARED_SIZE (0x8C3F) had no case in either glGetTexLevelParameter switch, so it fell
// into the terminal default arm and raised GL_INVALID_ENUM. esextcTextureCubeMapArrayGetterCalls
// walks a fixed pname list and TCU_FAILs on the first error, so the whole body died there even
// though every other pname it asks for was already implemented.
TEST_F(TextureTest, GetTexLevelParameterAnswersSharedSize) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    DrainPendingGlErrors();

    GLint sharedSize = -1;
    MG_Impl::GLImpl::GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_SHARED_SIZE, &sharedSize);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_EQ(sharedSize, 0) << "only a shared-exponent format has a shared exponent";

    GLfloat sharedSizeF = -1.0f;
    MG_Impl::GLImpl::GetTexLevelParameterfv(GL_TEXTURE_2D, 0, GL_TEXTURE_SHARED_SIZE, &sharedSizeF);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_FLOAT_EQ(sharedSizeF, 0.0f) << "the fv switch is a copy of the iv one and must not drift";

    // RGB9_E5 is the one format that HAS one, and it is five bits wide.
    GLuint sharedTexture = 0;
    MG_Impl::GLImpl::GenTextures(1, &sharedTexture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, sharedTexture);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGB9_E5, 4, 4, 0, GL_RGB, GL_FLOAT, nullptr);
    DrainPendingGlErrors();

    sharedSize = -1;
    MG_Impl::GLImpl::GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_SHARED_SIZE, &sharedSize);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_EQ(sharedSize, 5);

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, 0);
}

// glCopyTexSubImage3D was `{ // TODO: implement }` - no validation, no error, no copy - while its
// DSA sibling was fully implemented right next door. The two now share one body, so the target
// rules are the only thing that separates them.
TEST_F(TextureTest, CopyTexSubImage3DRejectsTargetsTheThreeDimensionalFormDoesNotTake) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    DrainPendingGlErrors();

    // GL 4.6 core 8.6: the 3D form takes only TEXTURE_3D / TEXTURE_2D_ARRAY /
    // TEXTURE_CUBE_MAP_ARRAY. A cube map's faces are two-dimensional targets and go through
    // glCopyTexSubImage2D. This used to be accepted silently, which is how the defect hid.
    MG_Impl::GLImpl::CopyTexSubImage3D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, 0, 2, 2);
    ExpectSingleGlError(GL_INVALID_ENUM);
    MG_Impl::GLImpl::CopyTexSubImage3D(GL_TEXTURE_CUBE_MAP, 0, 0, 0, 0, 0, 0, 2, 2);
    ExpectSingleGlError(GL_INVALID_ENUM);

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, 0);
}

TEST_F(TextureTest, CopyTexSubImage3DValidatesTheDestinationRegionOnACubeMapArray) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, texture);
    MG_Impl::GLImpl::TexImage3D(GL_TEXTURE_CUBE_MAP_ARRAY, 0, GL_RGBA8, 4, 4, 6, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                                nullptr);
    DrainPendingGlErrors();

    // A negative level is GL_INVALID_VALUE, and reaching it at all proves the entry point now
    // validates instead of returning silently.
    MG_Impl::GLImpl::CopyTexSubImage3D(GL_TEXTURE_CUBE_MAP_ARRAY, -1, 0, 0, 0, 0, 0, 2, 2);
    ExpectSingleGlError(GL_INVALID_VALUE);

    // So is a negative extent.
    MG_Impl::GLImpl::CopyTexSubImage3D(GL_TEXTURE_CUBE_MAP_ARRAY, 0, 0, 0, 0, 0, 0, -2, 2);
    ExpectSingleGlError(GL_INVALID_VALUE);

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, 0);
}

TEST_F(TextureTest, CopyTexSubImage1DRejectsAnythingButTexture1D) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    DrainPendingGlErrors();

    MG_Impl::GLImpl::CopyTexSubImage1D(GL_TEXTURE_2D, 0, 0, 0, 0, 2);
    ExpectSingleGlError(GL_INVALID_ENUM);

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, 0);
}

// ---------------------------------------------------------------------------------------------
// The buffer-texture entry points' error taxonomy (GL 4.6 core 8.9 / GL_EXT_texture_buffer).
// esextcTextureBufferErrors walks every OTHER texture target through glTexBuffer and
// glTexBufferRange and reads the code back each time, then does the same for a format a buffer
// texture cannot take. glTexBuffer carried `// TODO: make sure internalformat is in one of
// supported format for TexBuffer` and never checked, and the wrong-target code came out of a
// deeper "the bound object is not a buffer texture" arm whose code depends on which entry point
// reached it - GL_INVALID_OPERATION, which belongs only to the name-taking DSA forms.
// ---------------------------------------------------------------------------------------------

TEST_F(TextureTest, TexBufferAndTexBufferRangeRejectANonBufferTargetWithInvalidEnum) {
    GLuint buffer = 0;
    MG_Impl::GLImpl::GenBuffers(1, &buffer);
    MG_Impl::GLImpl::BindBuffer(GL_TEXTURE_BUFFER, buffer);
    MG_Impl::GLImpl::BufferData(GL_TEXTURE_BUFFER, 64, nullptr, GL_STATIC_DRAW);
    DrainPendingGlErrors();

    static constexpr GLenum kWrongTargets[] = {
        GL_TEXTURE_2D, GL_TEXTURE_2D_ARRAY, GL_TEXTURE_3D, GL_TEXTURE_CUBE_MAP, GL_TEXTURE_CUBE_MAP_ARRAY,
    };
    for (const GLenum target : kWrongTargets) {
        MG_Impl::GLImpl::TexBuffer(target, GL_RGBA32I, buffer);
        ExpectSingleGlError(GL_INVALID_ENUM);
        MG_Impl::GLImpl::TexBufferRange(target, GL_RGBA32I, buffer, 0, 64);
        ExpectSingleGlError(GL_INVALID_ENUM);
    }

    MG_Impl::GLImpl::BindBuffer(GL_TEXTURE_BUFFER, 0);
    MG_Impl::GLImpl::DeleteBuffers(1, &buffer);
    DrainPendingGlErrors();
}

TEST_F(TextureTest, TexBufferRejectsAnInternalFormatABufferTextureCannotTake) {
    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_BUFFER, texture);
    GLuint buffer = 0;
    MG_Impl::GLImpl::GenBuffers(1, &buffer);
    MG_Impl::GLImpl::BindBuffer(GL_TEXTURE_BUFFER, buffer);
    MG_Impl::GLImpl::BufferData(GL_TEXTURE_BUFFER, 64, nullptr, GL_STATIC_DRAW);
    DrainPendingGlErrors();

    // GL_DEPTH_COMPONENT32F is the one the conformance suite passes: a sized format, just not one
    // of the sized formats table 8.15 lists for a buffer texture.
    MG_Impl::GLImpl::TexBuffer(GL_TEXTURE_BUFFER, GL_DEPTH_COMPONENT32F, buffer);
    ExpectSingleGlError(GL_INVALID_ENUM);
    MG_Impl::GLImpl::TexBufferRange(GL_TEXTURE_BUFFER, GL_DEPTH_COMPONENT32F, buffer, 0, 64);
    ExpectSingleGlError(GL_INVALID_ENUM);

    // A format the table DOES list still goes through.
    MG_Impl::GLImpl::TexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32I, buffer);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_BUFFER, 0);
    MG_Impl::GLImpl::BindBuffer(GL_TEXTURE_BUFFER, 0);
    MG_Impl::GLImpl::DeleteBuffers(1, &buffer);
    DrainPendingGlErrors();
}

// The DSA form keeps its own, DIFFERENT code for the corresponding shape: a texture that is not a
// buffer texture is a wrong OBJECT, not a wrong token. The two must not be unified.
TEST_F(TextureTest, TextureBufferKeepsInvalidOperationForANonBufferTexture) {
    GLuint texture = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &texture);
    GLuint buffer = 0;
    MG_Impl::GLImpl::GenBuffers(1, &buffer);
    MG_Impl::GLImpl::BindBuffer(GL_TEXTURE_BUFFER, buffer);
    MG_Impl::GLImpl::BufferData(GL_TEXTURE_BUFFER, 64, nullptr, GL_STATIC_DRAW);
    DrainPendingGlErrors();

    MG_Impl::GLImpl::TextureBuffer(texture, GL_RGBA32I, buffer);
    ExpectSingleGlError(GL_INVALID_OPERATION);

    MG_Impl::GLImpl::BindBuffer(GL_TEXTURE_BUFFER, 0);
    MG_Impl::GLImpl::DeleteBuffers(1, &buffer);
    DrainPendingGlErrors();
}

// ---------------------------------------------------------------------------------------------
// The destination box of a copy is bounded by the LEVEL it writes, not by level 0.
//
// glCopyTexSubImage3D/1D validated through ValidateTextureSubImageOffsets, whose bound is
// ITextureObject::GetBaseSize() - hardcoded to level 0 - while CopyReadFramebufferIntoMipmapRegion
// indexes GetMipmapTexelSize(uploadTarget, level) and memcpys into the exact-sized allocation
// MipmapStorage made for that level. A box legal at level 0 and out of range at level N wrote past
// the end of the heap buffer. Both entry points were `// TODO: implement` no-ops before this
// branch, so implementing them is what opened the path.
//
// The region check runs BEFORE the read-framebuffer check on purpose, which is what lets this
// GPU-free binary assert it: no complete read FBO is needed to prove the box was rejected.
// ---------------------------------------------------------------------------------------------

TEST_F(TextureTest, CopyTexSubImage3DBoundsTheDestinationByTheRequestedLevelNotLevelZero) {
    GLuint texture = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D_ARRAY, 1, &texture);
    // 4 levels of an 8x8x4 array: level 0 is 8x8, level 1 4x4, level 2 2x2; the layer count stays
    // 4 at every level.
    MG_Impl::GLImpl::TextureStorage3D(texture, 4, GL_RGBA8, 8, 8, 4);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D_ARRAY, texture);
    DrainPendingGlErrors();

    // THE OVERFLOW: 4+4 <= 8 and 4+4 <= 8 against level 0, but level 2 is only 2x2. This used to
    // pass validation and write 24 bytes past a 64-byte allocation.
    MG_Impl::GLImpl::CopyTexSubImage3D(GL_TEXTURE_2D_ARRAY, 2, 4, 4, 0, 0, 0, 4, 4);
    ExpectSingleGlError(GL_INVALID_VALUE);

    // The same box one axis at a time, so a check that only looked at x or only at y cannot pass.
    MG_Impl::GLImpl::CopyTexSubImage3D(GL_TEXTURE_2D_ARRAY, 1, 3, 0, 0, 0, 0, 2, 2);
    ExpectSingleGlError(GL_INVALID_VALUE);
    MG_Impl::GLImpl::CopyTexSubImage3D(GL_TEXTURE_2D_ARRAY, 1, 0, 3, 0, 0, 0, 2, 2);
    ExpectSingleGlError(GL_INVALID_VALUE);

    // A box that IS inside level 1 (4x4) must get past the region check. It cannot complete here -
    // this binary has no complete read framebuffer - but it must not be the box that is refused.
    MG_Impl::GLImpl::CopyTexSubImage3D(GL_TEXTURE_2D_ARRAY, 1, 2, 2, 3, 0, 0, 2, 2);
    EXPECT_NE(MG_Impl::GLImpl::GetError(), GL_INVALID_VALUE)
        << "an in-range level-1 box must reach the framebuffer check, not be rejected as out of range";
    DrainPendingGlErrors();

    // The layer axis is bounded by the level's layer count, which does NOT shrink down the chain.
    MG_Impl::GLImpl::CopyTexSubImage3D(GL_TEXTURE_2D_ARRAY, 1, 0, 0, 4, 0, 0, 2, 2);
    ExpectSingleGlError(GL_INVALID_VALUE);

    // A level the texture never had is INVALID_OPERATION, not a write into an empty allocation.
    MG_Impl::GLImpl::CopyTexSubImage3D(GL_TEXTURE_2D_ARRAY, 5, 0, 0, 0, 0, 0, 1, 1);
    ExpectSingleGlError(GL_INVALID_OPERATION);

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D_ARRAY, 0);
    DrainPendingGlErrors();
}

TEST_F(TextureTest, CopyTexSubImage1DBoundsTheDestinationByTheRequestedLevel) {
    GLuint texture = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_1D, 1, &texture);
    MG_Impl::GLImpl::TextureStorage1D(texture, 4, GL_RGBA8, 8);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_1D, texture);
    DrainPendingGlErrors();

    // Level 2 is two texels, i.e. eight bytes; this used to write sixteen bytes starting sixteen
    // bytes in - entirely outside the allocation.
    MG_Impl::GLImpl::CopyTexSubImage1D(GL_TEXTURE_1D, 2, 4, 0, 0, 4);
    ExpectSingleGlError(GL_INVALID_VALUE);

    // In range at level 1 (four texels).
    MG_Impl::GLImpl::CopyTexSubImage1D(GL_TEXTURE_1D, 1, 2, 0, 0, 2);
    EXPECT_NE(MG_Impl::GLImpl::GetError(), GL_INVALID_VALUE);
    DrainPendingGlErrors();

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_1D, 0);
    DrainPendingGlErrors();
}

// glCopyTextureSubImage3D documents zoffset as the cube-map FACE selector, but the face mapping
// ran AFTER a z-bounds check taken from GetBaseSize().z(), which for a cube map is one face's
// depth - i.e. 1. Every zoffset in 1..5 was rejected with GL_INVALID_VALUE, so five of the six
// faces were unreachable. The mapping now runs first and is bounded by the face count.
TEST_F(TextureTest, CopyTextureSubImage3DCanAddressEveryCubeMapFace) {
    GLuint texture = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_CUBE_MAP, 1, &texture);
    MG_Impl::GLImpl::TextureStorage2D(texture, 2, GL_RGBA8, 4, 4);
    DrainPendingGlErrors();

    for (GLint face = 0; face < 6; ++face) {
        MG_Impl::GLImpl::CopyTextureSubImage3D(texture, 0, 0, 0, face, 0, 0, 4, 4);
        EXPECT_NE(MG_Impl::GLImpl::GetError(), GL_INVALID_VALUE)
            << "face " << face << " must be reachable; the z bound is the face count, not a face's depth";
        DrainPendingGlErrors();
    }

    // Past the last face is still GL_INVALID_VALUE.
    MG_Impl::GLImpl::CopyTextureSubImage3D(texture, 0, 0, 0, 6, 0, 0, 4, 4);
    ExpectSingleGlError(GL_INVALID_VALUE);
    MG_Impl::GLImpl::CopyTextureSubImage3D(texture, 0, 0, 0, -1, 0, 0, 4, 4);
    ExpectSingleGlError(GL_INVALID_VALUE);

    // And the per-FACE extent still bounds x/y at the requested level: level 1 of a 4x4 cube is
    // 2x2, so a 4x4 box into face 3 is out of range even though it fits level 0.
    MG_Impl::GLImpl::CopyTextureSubImage3D(texture, 1, 0, 0, 3, 0, 0, 4, 4);
    ExpectSingleGlError(GL_INVALID_VALUE);

    DrainPendingGlErrors();
}

// ---------------------------------------------------------------------------------------------
// glGenerateMipmap allocates the chain in the FRONTEND before it dispatches to the backend, and
// that allocator used a two-way "does depth mip?" flag which had no way to say that a 1D array's
// HEIGHT is its layer count. It therefore both counted the layer axis into the chain length and
// halved it per level. The backend allocator cannot repair that - it only ever GROWS a chain, and
// the frontend's (wrong) count is always the longer one - so the layer-shrinking chain survived on
// both backends, and ComputeMipmapCompleteForFilter (which knows height is not a dimension for
// this target) then judged the texture mipmap-INCOMPLETE, i.e. sampling returns (0,0,0,1).
// ---------------------------------------------------------------------------------------------

namespace {
    // glGenerateMipmap dispatches to the backend after the frontend allocation; this binary has no
    // GL context, so the hook is stubbed for the duration of the case. What is under test is the
    // frontend allocation the stub cannot influence.
    struct ScopedNoOpGenerateMipmap {
        ScopedNoOpGenerateMipmap(): m_snapshot(MobileGL::MG_Backend::gBackendFunctionsTable) {
            MobileGL::MG_Backend::gBackendFunctionsTable.GL.GenerateMipmap = [](GLenum) {};
        }
        ~ScopedNoOpGenerateMipmap() { MobileGL::MG_Backend::gBackendFunctionsTable = m_snapshot; }
        ScopedNoOpGenerateMipmap(const ScopedNoOpGenerateMipmap&) = delete;
        ScopedNoOpGenerateMipmap& operator=(const ScopedNoOpGenerateMipmap&) = delete;

    private:
        MobileGL::MG_Backend::GlobalBackendFunctionsTable m_snapshot;
    };

    GLint LevelParam(GLenum target, GLint level, GLenum pname) {
        GLint value = -1;
        MG_Impl::GLImpl::GetTexLevelParameteriv(target, level, pname, &value);
        return value;
    }
} // namespace

TEST_F(TextureTest, GenerateMipmapKeepsA1DArrayLayerCountAtEveryLevel) {
    ScopedNoOpGenerateMipmap noOpBackend;

    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_1D_ARRAY, texture);
    // Width 8, FOUR layers. The layer count is carried in `height` for this target.
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_1D_ARRAY, 0, GL_RGBA8, 8, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    DrainPendingGlErrors();

    MG_Impl::GLImpl::GenerateMipmap(GL_TEXTURE_1D_ARRAY);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // The chain length comes from the WIDTH alone: 8 -> 4 -> 2 -> 1 is four levels. Counting the
    // layer axis too would give the same four here, so the width is chosen larger than the layer
    // count on purpose and the layer assertions below are what actually discriminate.
    for (GLint level = 0; level < 4; ++level) {
        EXPECT_EQ(LevelParam(GL_TEXTURE_1D_ARRAY, level, GL_TEXTURE_WIDTH), std::max(8 >> level, 1))
            << "level " << level << " width";
        EXPECT_EQ(LevelParam(GL_TEXTURE_1D_ARRAY, level, GL_TEXTURE_HEIGHT), 4)
            << "level " << level << " must keep all four layers; height is the layer count for a 1D array";
    }
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_1D_ARRAY, 0);
    DrainPendingGlErrors();
}

// The mirror case: more layers than texels. The chain must be as long as the WIDTH admits, not as
// long as the layer count admits - a chain sized off the layers would allocate levels whose width
// has already bottomed out at 1 while the layer count kept halving.
TEST_F(TextureTest, GenerateMipmapSizesA1DArrayChainFromWidthAloneEvenWithMoreLayersThanTexels) {
    ScopedNoOpGenerateMipmap noOpBackend;

    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_1D_ARRAY, texture);
    // Width 2, sixteen layers: counting the layer axis would ask for five levels, the width for two.
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_1D_ARRAY, 0, GL_RGBA8, 2, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    DrainPendingGlErrors();

    MG_Impl::GLImpl::GenerateMipmap(GL_TEXTURE_1D_ARRAY);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    EXPECT_EQ(LevelParam(GL_TEXTURE_1D_ARRAY, 1, GL_TEXTURE_WIDTH), 1);
    EXPECT_EQ(LevelParam(GL_TEXTURE_1D_ARRAY, 1, GL_TEXTURE_HEIGHT), 16);
    // Level 2 must not exist: the chain ends where the width does.
    EXPECT_EQ(LevelParam(GL_TEXTURE_1D_ARRAY, 2, GL_TEXTURE_WIDTH), 0);
    DrainPendingGlErrors();

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_1D_ARRAY, 0);
    DrainPendingGlErrors();
}

// The 2D-array/cube-array side of the same rule, so a fix that swung the other way (making depth
// mip-able again) cannot pass. Depth is the layer count for these; only width and height reduce.
TEST_F(TextureTest, GenerateMipmapKeepsA2DArrayLayerCountAtEveryLevel) {
    ScopedNoOpGenerateMipmap noOpBackend;

    GLuint texture = 0;
    MG_Impl::GLImpl::GenTextures(1, &texture);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D_ARRAY, texture);
    MG_Impl::GLImpl::TexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 8, 8, 3, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    DrainPendingGlErrors();

    MG_Impl::GLImpl::GenerateMipmap(GL_TEXTURE_2D_ARRAY);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    for (GLint level = 0; level < 4; ++level) {
        EXPECT_EQ(LevelParam(GL_TEXTURE_2D_ARRAY, level, GL_TEXTURE_WIDTH), std::max(8 >> level, 1));
        EXPECT_EQ(LevelParam(GL_TEXTURE_2D_ARRAY, level, GL_TEXTURE_HEIGHT), std::max(8 >> level, 1));
        EXPECT_EQ(LevelParam(GL_TEXTURE_2D_ARRAY, level, GL_TEXTURE_DEPTH), 3)
            << "level " << level << " must keep all three layers";
    }
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // And a true 3D texture still halves all three, which is the case the layer rule must not eat.
    GLuint volume = 0;
    MG_Impl::GLImpl::GenTextures(1, &volume);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_3D, volume);
    MG_Impl::GLImpl::TexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, 8, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    DrainPendingGlErrors();
    MG_Impl::GLImpl::GenerateMipmap(GL_TEXTURE_3D);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_EQ(LevelParam(GL_TEXTURE_3D, 1, GL_TEXTURE_DEPTH), 4);

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_3D, 0);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D_ARRAY, 0);
    DrainPendingGlErrors();
}
