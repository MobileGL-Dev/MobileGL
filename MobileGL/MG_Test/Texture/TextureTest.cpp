// MobileGL - MobileGL/MG_Test/Texture/TextureTest.cpp
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
#include <MG_Impl/GLImpl/Getter/GL_Getter.h>
#include <MG_Impl/GLImpl/RenderState/GL_RenderState.h>
#include <MG_Impl/GLImpl/Texture/GL_Texture.h>
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/TextureState/TextureObject.h>
#include <MG_Util/Converters/MGToMG/TextureEnumConverter.h>
#include <MG_Util/Texture/TextureFormatProcessor.h>

using namespace MobileGL;

class TextureTest : public ::testing::Test {
protected:
    void SetUp() override { MobileGL::Initialize(); }
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
} // namespace

TEST_F(TextureTest, CreateTexturesCreatesObjectsWithoutBinding) {
    GLuint texture = 0;
    MG_Impl::GLImpl::CreateTextures(GL_TEXTURE_2D, 1, &texture);

    EXPECT_NE(texture, 0u);
    EXPECT_TRUE(MG_State::pGLContext->ValidateTextureObject(texture));

    auto& unit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
    EXPECT_EQ(unit.GetBindingSlot(TextureTarget::Texture2D).GetBoundObject(), nullptr);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
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
