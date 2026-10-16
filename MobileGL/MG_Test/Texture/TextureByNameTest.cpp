// MobileGL - MobileGL/MG_Test/Texture/TextureByNameTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The by-name (DSA) texture entry points edit the named texture directly: no unit binding and no
// bind generation moves, and each checks the texture's effective target the way GL 4.6 core asks of
// the by-name spelling - which is not always what the bound-target spelling accepts.
#include <gtest/gtest.h>

#include <cstring>

#include "Includes.h"
#include "Init.h"
#include <MG_Backend/BackendObjects.h>
#include <MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.h>
#include <MG_Impl/GLImpl/Getter/GL_Getter.h>
#include <MG_Impl/GLImpl/Texture/GL_Texture.h>
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/TextureState/TextureObject.h>

using namespace MobileGL;

class TextureByNameTest : public ::testing::Test {
protected:
    static void DrainPendingGlErrors() {
        for (Int drained = 0; drained < 16 && MG_Impl::GLImpl::GetError() != GL_NO_ERROR; ++drained) {}
    }

    static void ExpectSingleGlError(GLenum expected) {
        EXPECT_EQ(MG_Impl::GLImpl::GetError(), expected);
        EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR) << "the call recorded more than one error";
    }

    void SetUp() override {
        MobileGL::Initialize();
        DrainPendingGlErrors();
    }

    void TearDown() override {
        EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR) << "test left an unconsumed GL error behind";
    }
};

namespace {
    // This binary has no GL context behind its backend table, so the two calls that reach the backend
    // are stubbed for the duration of a case; what is under test is what the frontend does first.
    class ScopedBackendStubs {
    public:
        ScopedBackendStubs() : m_snapshot(MG_Backend::gBackendFunctionsTable) {
            auto& gl = MG_Backend::gBackendFunctionsTable.GL;
            gl.GenerateTextureMipmap = [](const SharedPtr<MG_State::GLState::ITextureObject>&, GLenum) {};
            gl.CopyTextureSubImage2D = [](const SharedPtr<MG_State::GLState::ITextureObject>&, GLenum, GLint, GLint,
                                          GLint, GLint, GLint, GLsizei, GLsizei) {};
        }
        ~ScopedBackendStubs() { MG_Backend::gBackendFunctionsTable = m_snapshot; }
        ScopedBackendStubs(const ScopedBackendStubs&) = delete;
        ScopedBackendStubs& operator=(const ScopedBackendStubs&) = delete;

    private:
        MG_Backend::GlobalBackendFunctionsTable m_snapshot;
    };

    // What a by-name call must leave as it found it: every slot of the active unit and the bind
    // generation backends key their per-unit memos on.
    struct UnitState {
        Uint64 generation = 0;
        Vector<MG_State::GLState::ITextureObject*> slots;

        static UnitState Capture() {
            UnitState state;
            state.generation = MG_State::pGLContext->GetTextureBindGeneration();
            auto& unit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
            for (TextureTarget target :
                 {TextureTarget::Texture1D, TextureTarget::Texture2D, TextureTarget::Texture3D,
                  TextureTarget::Texture1DArray, TextureTarget::Texture2DArray, TextureTarget::TextureCubeMap,
                  TextureTarget::Texture2DMultisample, TextureTarget::Texture2DMultisampleArray}) {
                state.slots.push_back(unit.GetBindingSlot(target).GetBoundObject().get());
            }
            return state;
        }
        Bool operator==(const UnitState& other) const { return generation == other.generation && slots == other.slots; }
    };

    GLuint CreateTexture(GLenum target) {
        GLuint texture = 0;
        MG_Impl::GLImpl::CreateTextures(target, 1, &texture);
        return texture;
    }

    // The CPU texel shadow of one image of a named texture.
    const Uint8* ImageBytes(GLuint texture, TextureUploadTarget image, Uint level = 0) {
        auto* mipmap = MG_State::GLState::AsMipmapTexture(MG_State::pGLContext->GetTextureObject(texture).get());
        return mipmap ? static_cast<const Uint8*>(mipmap->MapMipmapData(image, level)) : nullptr;
    }

    TextureUploadTarget CubeFace(Int face) {
        return static_cast<TextureUploadTarget>(static_cast<SizeT>(TextureUploadTarget::CubeMapPositiveX) +
                                                static_cast<SizeT>(face));
    }

    constexpr GLsizei kRgtc1Block = 8; // one 4x4 block of GL_COMPRESSED_RED_RGTC1
} // namespace

TEST_F(TextureByNameTest, EveryByNameCallLeavesTheUnitAndTheBindGenerationAlone) {
    const ScopedBackendStubs stubs;
    // Decoys on every target of the active unit, so a call that bound its texture would show.
    for (GLenum target : {GL_TEXTURE_1D, GL_TEXTURE_2D, GL_TEXTURE_3D, GL_TEXTURE_2D_ARRAY, GL_TEXTURE_CUBE_MAP}) {
        GLuint decoy = 0;
        MG_Impl::GLImpl::GenTextures(1, &decoy);
        MG_Impl::GLImpl::BindTexture(target, decoy);
    }
    const GLuint tex1D = CreateTexture(GL_TEXTURE_1D);
    MG_Impl::GLImpl::TextureStorage1D(tex1D, 1, GL_RGBA8, 4);
    const GLuint tex2D = CreateTexture(GL_TEXTURE_2D);
    MG_Impl::GLImpl::TextureStorage2D(tex2D, 3, GL_RGBA8, 4, 4);
    const GLuint texArray = CreateTexture(GL_TEXTURE_2D_ARRAY);
    MG_Impl::GLImpl::TextureStorage3D(texArray, 1, GL_RGBA8, 4, 4, 2);
    const GLuint texMs = CreateTexture(GL_TEXTURE_2D_MULTISAMPLE);
    const GLuint texMsArray = CreateTexture(GL_TEXTURE_2D_MULTISAMPLE_ARRAY);
    GLuint readFramebuffer = 0;
    MG_Impl::GLImpl::CreateFramebuffers(1, &readFramebuffer);
    const GLuint readTexture = CreateTexture(GL_TEXTURE_2D);
    MG_Impl::GLImpl::TextureStorage2D(readTexture, 1, GL_RGBA8, 4, 4);
    MG_Impl::GLImpl::NamedFramebufferTexture(readFramebuffer, GL_COLOR_ATTACHMENT0, readTexture, 0);
    MG_Impl::GLImpl::BindFramebuffer(GL_READ_FRAMEBUFFER, readFramebuffer);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    const Uint8 pixels[4 * 4 * 4 * 2] = {};
    const GLfloat border[4] = {0, 0, 0, 1};
    const GLint swizzle[4] = {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA};
    const GLuint swizzleU[4] = {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA};
    GLint ints[4] = {};
    GLuint uints[4] = {};
    GLfloat floats[4] = {};

    const auto expectUntouched = [](const char* name, const auto& call) {
        const UnitState before = UnitState::Capture();
        call();
        EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR) << name;
        EXPECT_TRUE(UnitState::Capture() == before) << name << " moved a binding or the bind generation";
    };
    expectUntouched("TextureParameterfv",
                    [&] { MG_Impl::GLImpl::TextureParameterfv(tex2D, GL_TEXTURE_BORDER_COLOR, border); });
    expectUntouched("TextureParameteriv",
                    [&] { MG_Impl::GLImpl::TextureParameteriv(tex2D, GL_TEXTURE_SWIZZLE_RGBA, swizzle); });
    expectUntouched("TextureParameterIiv",
                    [&] { MG_Impl::GLImpl::TextureParameterIiv(tex2D, GL_TEXTURE_SWIZZLE_RGBA, swizzle); });
    expectUntouched("TextureParameterIuiv",
                    [&] { MG_Impl::GLImpl::TextureParameterIuiv(tex2D, GL_TEXTURE_SWIZZLE_RGBA, swizzleU); });
    expectUntouched("GetTextureParameteriv",
                    [&] { MG_Impl::GLImpl::GetTextureParameteriv(tex2D, GL_TEXTURE_MAX_LEVEL, ints); });
    expectUntouched("GetTextureParameterfv",
                    [&] { MG_Impl::GLImpl::GetTextureParameterfv(tex2D, GL_TEXTURE_MIN_LOD, floats); });
    expectUntouched("GetTextureParameterIiv",
                    [&] { MG_Impl::GLImpl::GetTextureParameterIiv(tex2D, GL_TEXTURE_BORDER_COLOR, ints); });
    expectUntouched("GetTextureParameterIuiv",
                    [&] { MG_Impl::GLImpl::GetTextureParameterIuiv(tex2D, GL_TEXTURE_BORDER_COLOR, uints); });
    expectUntouched("GetTextureLevelParameteriv",
                    [&] { MG_Impl::GLImpl::GetTextureLevelParameteriv(tex2D, 1, GL_TEXTURE_WIDTH, ints); });
    expectUntouched("GetTextureLevelParameterfv",
                    [&] { MG_Impl::GLImpl::GetTextureLevelParameterfv(tex2D, 1, GL_TEXTURE_HEIGHT, floats); });
    expectUntouched("TextureSubImage1D",
                    [&] { MG_Impl::GLImpl::TextureSubImage1D(tex1D, 0, 0, 4, GL_RGBA, GL_UNSIGNED_BYTE, pixels); });
    expectUntouched("TextureSubImage2D", [&] {
        MG_Impl::GLImpl::TextureSubImage2D(tex2D, 0, 0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    });
    expectUntouched("TextureSubImage3D", [&] {
        MG_Impl::GLImpl::TextureSubImage3D(texArray, 0, 0, 0, 0, 4, 4, 2, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    });
    expectUntouched("TextureStorage2DMultisample",
                    [&] { MG_Impl::GLImpl::TextureStorage2DMultisample(texMs, 1, GL_RGBA8, 4, 4, GL_TRUE); });
    expectUntouched("TextureStorage3DMultisample",
                    [&] { MG_Impl::GLImpl::TextureStorage3DMultisample(texMsArray, 1, GL_RGBA8, 4, 4, 2, GL_TRUE); });
    expectUntouched("GenerateTextureMipmap", [&] { MG_Impl::GLImpl::GenerateTextureMipmap(tex2D); });
    expectUntouched("CopyTextureSubImage2D",
                    [&] { MG_Impl::GLImpl::CopyTextureSubImage2D(tex2D, 0, 0, 0, 0, 0, 2, 2); });

    MG_Impl::GLImpl::BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    DrainPendingGlErrors();
}

// The compressed by-name uploads, with textures whose images are stored compressed.
TEST_F(TextureByNameTest, CompressedByNameUploadsLeaveTheUnitAndTheBindGenerationAlone) {
    const GLuint tex2D = CreateTexture(GL_TEXTURE_2D);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, tex2D);
    const Uint8 blocks[kRgtc1Block * 2] = {};
    MG_Impl::GLImpl::CompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RED_RGTC1, 4, 4, 0, kRgtc1Block, blocks);
    const GLuint texArray = CreateTexture(GL_TEXTURE_2D_ARRAY);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D_ARRAY, texArray);
    MG_Impl::GLImpl::CompressedTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_COMPRESSED_RED_RGTC1, 4, 4, 2, 0, kRgtc1Block * 2,
                                          blocks);
    GLuint decoys[2] = {};
    MG_Impl::GLImpl::GenTextures(2, decoys);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, decoys[0]);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D_ARRAY, decoys[1]);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    UnitState before = UnitState::Capture();
    MG_Impl::GLImpl::CompressedTextureSubImage2D(tex2D, 0, 0, 0, 4, 4, GL_COMPRESSED_RED_RGTC1, kRgtc1Block, blocks);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_TRUE(UnitState::Capture() == before);

    before = UnitState::Capture();
    MG_Impl::GLImpl::CompressedTextureSubImage3D(texArray, 0, 0, 0, 0, 4, 4, 2, GL_COMPRESSED_RED_RGTC1,
                                                 kRgtc1Block * 2, blocks);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_TRUE(UnitState::Capture() == before);
}

// GL 4.6 core 8.6: the by-name 3D upload takes a cube map, zoffset naming the first face and depth
// the number of faces. The bound-target path it used to borrow has no target for that.
TEST_F(TextureByNameTest, TextureSubImage3DWritesTheCubeFacesZOffsetSelects) {
    const GLuint cube = CreateTexture(GL_TEXTURE_CUBE_MAP);
    MG_Impl::GLImpl::TextureStorage2D(cube, 1, GL_RGBA8, 2, 2);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    Uint8 faces[2][2 * 2 * 4];
    std::memset(faces[0], 0x40, sizeof(faces[0]));
    std::memset(faces[1], 0x80, sizeof(faces[1]));
    MG_Impl::GLImpl::TextureSubImage3D(cube, 0, 0, 0, 2, 2, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, faces);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    for (Int face = 0; face < 6; ++face) {
        const Uint8* bytes = ImageBytes(cube, CubeFace(face));
        ASSERT_NE(bytes, nullptr) << "face " << face;
        const Uint8 expected = face == 2 ? 0x40 : (face == 3 ? 0x80 : 0);
        for (SizeT i = 0; i < sizeof(faces[0]); ++i) {
            ASSERT_EQ(bytes[i], expected) << "face " << face << " byte " << i;
        }
    }

    // Faces past the sixth do not exist.
    MG_Impl::GLImpl::TextureSubImage3D(cube, 0, 0, 0, 5, 2, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, faces);
    ExpectSingleGlError(GL_INVALID_VALUE);
}

TEST_F(TextureByNameTest, TextureSubImage3DIntoACubeMapThatIsNotCubeCompleteIsInvalidOperation) {
    const GLuint cube = CreateTexture(GL_TEXTURE_CUBE_MAP);
    const Uint8 pixels[2 * 2 * 4] = {};
    MG_Impl::GLImpl::TextureSubImage3D(cube, 0, 0, 0, 0, 2, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    ExpectSingleGlError(GL_INVALID_OPERATION);
}

TEST_F(TextureByNameTest, CompressedTextureSubImage3DWritesTheCubeFacesZOffsetSelects) {
    const GLuint cube = CreateTexture(GL_TEXTURE_CUBE_MAP);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_CUBE_MAP, cube);
    const Uint8 zero[kRgtc1Block] = {};
    for (GLenum face = GL_TEXTURE_CUBE_MAP_POSITIVE_X; face <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z; ++face) {
        MG_Impl::GLImpl::CompressedTexImage2D(face, 0, GL_COMPRESSED_RED_RGTC1, 4, 4, 0, kRgtc1Block, zero);
    }
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    Uint8 blocks[2][kRgtc1Block];
    std::memset(blocks[0], 0x11, sizeof(blocks[0]));
    std::memset(blocks[1], 0x22, sizeof(blocks[1]));
    MG_Impl::GLImpl::CompressedTextureSubImage3D(cube, 0, 0, 0, 1, 4, 4, 2, GL_COMPRESSED_RED_RGTC1, kRgtc1Block * 2,
                                                 blocks);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    for (Int face = 0; face < 6; ++face) {
        Uint8 stored[kRgtc1Block] = {};
        MG_Impl::GLImpl::GetCompressedTexImage(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, stored);
        const Uint8 expected = face == 1 ? 0x11 : (face == 2 ? 0x22 : 0);
        for (SizeT i = 0; i < sizeof(stored); ++i) {
            ASSERT_EQ(stored[i], expected) << "face " << face << " byte " << i;
        }
    }
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// GL 4.6 core table 8.15. A target the command does not take is INVALID_ENUM through a target
// token and INVALID_OPERATION through a named texture's effective target.
TEST_F(TextureByNameTest, SubImageCommandsCheckTheirTargetTable) {
    const Uint8 pixels[4 * 4 * 4] = {};
    const GLuint cube = CreateTexture(GL_TEXTURE_CUBE_MAP);
    MG_Impl::GLImpl::TextureStorage2D(cube, 1, GL_RGBA8, 4, 4);
    const GLuint tex2D = CreateTexture(GL_TEXTURE_2D);
    MG_Impl::GLImpl::TextureStorage2D(tex2D, 1, GL_RGBA8, 4, 4);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::TextureSubImage2D(cube, 0, 0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    ExpectSingleGlError(GL_INVALID_OPERATION);
    MG_Impl::GLImpl::TextureSubImage3D(tex2D, 0, 0, 0, 0, 4, 4, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    ExpectSingleGlError(GL_INVALID_OPERATION);
    MG_Impl::GLImpl::TextureSubImage1D(tex2D, 0, 0, 4, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    ExpectSingleGlError(GL_INVALID_OPERATION);

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, tex2D);
    MG_Impl::GLImpl::TexSubImage3D(GL_TEXTURE_2D, 0, 0, 0, 0, 4, 4, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    ExpectSingleGlError(GL_INVALID_ENUM);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_CUBE_MAP, cube);
    MG_Impl::GLImpl::TexSubImage2D(GL_TEXTURE_CUBE_MAP, 0, 0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    ExpectSingleGlError(GL_INVALID_ENUM);
    MG_Impl::GLImpl::TexSubImage2D(GL_PROXY_TEXTURE_2D, 0, 0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    ExpectSingleGlError(GL_INVALID_ENUM);

    // A face takes any rectangle inside it: the square-face rule is image specification's.
    MG_Impl::GLImpl::TexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_Y, 0, 0, 0, 4, 2, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// The box is bounded by the level it writes, not by level 0: at level 1 of a 4x4 texture a 2x2
// box at (1, 1) runs off the image, which is INVALID_VALUE rather than a silently dropped upload.
TEST_F(TextureByNameTest, SubImageOutsideTheLevelItWritesIsInvalidValue) {
    const Uint8 pixels[2 * 2 * 4] = {};
    const GLuint texture = CreateTexture(GL_TEXTURE_2D);
    MG_Impl::GLImpl::TextureStorage2D(texture, 2, GL_RGBA8, 4, 4);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Impl::GLImpl::TextureSubImage2D(texture, 1, 1, 1, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    ExpectSingleGlError(GL_INVALID_VALUE);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
    MG_Impl::GLImpl::TexSubImage2D(GL_TEXTURE_2D, 1, 1, 1, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    ExpectSingleGlError(GL_INVALID_VALUE);
    MG_Impl::GLImpl::TexSubImage2D(GL_TEXTURE_2D, 1, 0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// GL 4.6 core 8.11.3: by name a cube map answers level queries for face zero; through a target
// token GL_TEXTURE_CUBE_MAP is not a level-query target at all.
TEST_F(TextureByNameTest, LevelQueriesOfACubeMapByNameAnswerForFaceZero) {
    const GLuint cube = CreateTexture(GL_TEXTURE_CUBE_MAP);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_CUBE_MAP, cube);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X, 0, GL_RGBA8, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                                nullptr);
    MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_CUBE_MAP_NEGATIVE_X, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                                nullptr);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    GLint width = 0;
    MG_Impl::GLImpl::GetTextureLevelParameteriv(cube, 0, GL_TEXTURE_WIDTH, &width);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_EQ(width, 8);
    GLfloat height = 0;
    MG_Impl::GLImpl::GetTextureLevelParameterfv(cube, 0, GL_TEXTURE_HEIGHT, &height);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_EQ(height, 8.0f);

    MG_Impl::GLImpl::GetTexLevelParameteriv(GL_TEXTURE_CUBE_MAP, 0, GL_TEXTURE_WIDTH, &width);
    ExpectSingleGlError(GL_INVALID_ENUM);
}

// GL 4.6 core 8.11.2: TEXTURE_TARGET answers the effective target by name and the target itself
// through a token.
TEST_F(TextureByNameTest, ParameterQueriesAnswerTheTextureTarget) {
    const GLuint array = CreateTexture(GL_TEXTURE_2D_ARRAY);
    GLint target = 0;
    MG_Impl::GLImpl::GetTextureParameteriv(array, GL_TEXTURE_TARGET, &target);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_EQ(target, GL_TEXTURE_2D_ARRAY);

    GLuint bound = 0;
    MG_Impl::GLImpl::GenTextures(1, &bound);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_3D, bound);
    GLfloat floatTarget = 0;
    MG_Impl::GLImpl::GetTexParameterfv(GL_TEXTURE_3D, GL_TEXTURE_TARGET, &floatTarget);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_EQ(floatTarget, static_cast<GLfloat>(GL_TEXTURE_3D));
}

// The float setters of both spellings take every scalar pname the integer ones take: the bound
// form once refused DEPTH_STENCIL_TEXTURE_MODE and the by-name form the single-channel swizzles.
TEST_F(TextureByNameTest, FloatSettersTakeEveryScalarPnameOfBothForms) {
    const GLuint texture = CreateTexture(GL_TEXTURE_2D);
    MG_Impl::GLImpl::TextureParameterf(texture, GL_TEXTURE_SWIZZLE_G, static_cast<GLfloat>(GL_RED));
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    GLint swizzle = 0;
    MG_Impl::GLImpl::GetTextureParameteriv(texture, GL_TEXTURE_SWIZZLE_G, &swizzle);
    EXPECT_EQ(swizzle, GL_RED);

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
    MG_Impl::GLImpl::TexParameterf(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE,
                                   static_cast<GLfloat>(GL_STENCIL_INDEX));
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    GLint mode = 0;
    MG_Impl::GLImpl::GetTexParameteriv(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, &mode);
    EXPECT_EQ(mode, GL_STENCIL_INDEX);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}

// GL 4.6 core 8.14.4: mipmaps are generated for seven targets; a rectangle texture is not one.
TEST_F(TextureByNameTest, MipmapGenerationChecksTheTargetOfBothForms) {
    const ScopedBackendStubs stubs;
    const GLuint rectangle = CreateTexture(GL_TEXTURE_RECTANGLE);
    MG_Impl::GLImpl::GenerateTextureMipmap(rectangle);
    ExpectSingleGlError(GL_INVALID_OPERATION);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_RECTANGLE, rectangle);
    MG_Impl::GLImpl::GenerateMipmap(GL_TEXTURE_RECTANGLE);
    ExpectSingleGlError(GL_INVALID_ENUM);
}

// GENERATE_MIPMAP belongs to the texture it was set on. GL names are recycled, so the next texture
// created under a deleted texture's name must start without the flag.
TEST_F(TextureByNameTest, AutoMipmapFlagDoesNotOutliveItsTexture) {
    const ScopedBackendStubs stubs;
    static Int generated;
    generated = 0;
    MG_Backend::gBackendFunctionsTable.GL.GenerateTextureMipmap =
        [](const SharedPtr<MG_State::GLState::ITextureObject>&, GLenum) { ++generated; };
    const Uint8 pixels[4 * 4 * 4] = {};

    const GLuint deleted = CreateTexture(GL_TEXTURE_2D);
    MG_Impl::GLImpl::TextureParameteri(deleted, GL_GENERATE_MIPMAP, GL_TRUE);
    MG_Impl::GLImpl::TextureStorage2D(deleted, 3, GL_RGBA8, 4, 4);
    MG_Impl::GLImpl::TextureSubImage2D(deleted, 0, 0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    ASSERT_EQ(generated, 1) << "the flag did not generate mipmaps on its own texture";
    MG_Impl::GLImpl::DeleteTextures(1, &deleted);

    const GLuint recycled = CreateTexture(GL_TEXTURE_2D);
    ASSERT_EQ(recycled, deleted) << "the name was not recycled, so nothing here is tested";
    MG_Impl::GLImpl::TextureStorage2D(recycled, 3, GL_RGBA8, 4, 4);
    MG_Impl::GLImpl::TextureSubImage2D(recycled, 0, 0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_EQ(generated, 1) << "the new texture inherited the deleted texture's GENERATE_MIPMAP";
}
