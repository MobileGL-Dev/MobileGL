// MobileGL - MobileGL/MG_Test/Texture/TextureViewTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// The frontend half of glTextureView (ARB_texture_view / GL 4.6 core 8.18): its error surface and
// the state it derives. The cases below are the conformance suite's own list
// (KHR-GL43.texture_view.errors, a..s) plus the composition rules 8.18 spells out, which
// KHR-GL43.texture_view.gettexparameter checks.
//
// No backend is involved: glTextureView creates a frontend texture object whose storage is
// another object's, and everything asserted here is decided before any driver sees it. The one
// backend fact that matters is whether the active backend can share storage between two texture
// names at all - MobileGL keys that on the advertised GL_ARB_texture_view string, so the fixture
// installs a backend that advertises it (and one test removes it again, to pin the refusal).

#include <gtest/gtest.h>

#include <algorithm>

#include "Includes.h"
#include "Init.h"
#include <MG_Backend/BackendObjects.h>
#include <MG_Impl/GLImpl/Getter/GL_Getter.h>
#include <MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.h>
#include <MG_Impl/GLImpl/Texture/GL_Texture.h>
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/TextureState/TextureObject.h>
#include <MG_Util/GLExtensions.h>

using namespace MobileGL;

namespace {
    // A backend that claims exactly one thing: whether it can back a texture view. Everything
    // else about it is inert, because nothing else in this file reaches a backend.
    class TextureViewCapabilityBackend final : public MG_Backend::BackendObject {
    public:
        explicit TextureViewCapabilityBackend(Bool advertiseTextureView) {
            m_info.RendererName = "TextureViewTest";
            if (advertiseTextureView) {
                m_info.RendererGLInfo.Extensions.push_back(E_GL_ARB_texture_view);
            }
        }

        void Initialize() override {}
        Bool InitCapabilities() override { return true; }
        Bool InitWindowSurface() override { return true; }
        const RendererInfo& GetRendererInfo() const override { return m_info; }
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

    private:
        RendererInfo m_info;
    };

    class ScopedBackendOverride {
    public:
        explicit ScopedBackendOverride(Bool advertiseTextureView)
            : m_previous(Move(MG_Backend::pActiveBackendObject)) {
            MG_Backend::pActiveBackendObject = MakeUnique<TextureViewCapabilityBackend>(advertiseTextureView);
        }
        ~ScopedBackendOverride() { MG_Backend::pActiveBackendObject = Move(m_previous); }

    private:
        UniquePtr<MG_Backend::BackendObject> m_previous;
    };

    class TextureViewTest : public ::testing::Test {
    protected:
        // GL error flags are sticky per error code and the context outlives an individual test in
        // this binary, so anything an earlier test left pending would be handed to the next
        // GetError() call - which silently turns error-code assertions into reads of someone
        // else's error. Bounded because there is one flag per code.
        static void DrainPendingGlErrors() {
            for (Int drained = 0; drained < 16 && MG_Impl::GLImpl::GetError() != GL_NO_ERROR; ++drained) {
            }
        }

        // The call under test must raise exactly the expected error and nothing more: a second
        // pending error means one entry point queued several, which GetError() would hand out at
        // an unrelated call site later on.
        static void ExpectSingleGlError(GLenum expected) {
            EXPECT_EQ(MG_Impl::GLImpl::GetError(), expected);
            EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR) << "the call recorded more than one error";
        }

        void SetUp() override {
            MobileGL::Initialize();
            DrainPendingGlErrors();
            m_backend = MakeUnique<ScopedBackendOverride>(true);
        }

        void TearDown() override {
            m_backend.reset();
            EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR) << "test left an unconsumed GL error behind";
        }

        static GLuint GenTexture() {
            GLuint texture = 0;
            MG_Impl::GLImpl::GenTextures(1, &texture);
            return texture;
        }

        // A bound, immutable GL_TEXTURE_2D. `levels` levels of `size` x `size` RGBA8 unless a
        // caller wants otherwise.
        static GLuint MakeImmutable2D(GLsizei levels = 2, GLsizei width = 16, GLsizei height = 16,
                                      GLenum internalFormat = GL_RGBA8) {
            const GLuint texture = GenTexture();
            MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
            MG_Impl::GLImpl::TexStorage2D(GL_TEXTURE_2D, levels, internalFormat, width, height);
            return texture;
        }

        static GLuint MakeImmutable2DArray(GLsizei levels = 1, GLsizei size = 16, GLsizei layers = 6) {
            const GLuint texture = GenTexture();
            MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D_ARRAY, texture);
            MG_Impl::GLImpl::TexStorage3D(GL_TEXTURE_2D_ARRAY, levels, GL_RGBA8, size, size, layers);
            return texture;
        }

        static GLint GetViewParameter(GLuint texture, GLenum target, GLenum pname) {
            GLint value = -1;
            MG_Impl::GLImpl::BindTexture(target, texture);
            MG_Impl::GLImpl::GetTexParameteriv(target, pname, &value);
            return value;
        }

        UniquePtr<ScopedBackendOverride> m_backend;
    };

    // ============================ the state TexStorage* seeds ============================
    // GL 4.6 core 8.19 leaves an immutable texture describing itself as a full-extent view of its
    // own storage. That is not cosmetic: glTextureView COMPOSES onto these values, so if they
    // stayed at the mutable default of 0 every view would clamp to zero levels.

    TEST_F(TextureViewTest, MutableTextureReportsNoViewState) {
        const GLuint texture = GenTexture();
        MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
        MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        DrainPendingGlErrors();

        EXPECT_EQ(GetViewParameter(texture, GL_TEXTURE_2D, GL_TEXTURE_VIEW_MIN_LEVEL), 0);
        EXPECT_EQ(GetViewParameter(texture, GL_TEXTURE_2D, GL_TEXTURE_VIEW_NUM_LEVELS), 0);
        EXPECT_EQ(GetViewParameter(texture, GL_TEXTURE_2D, GL_TEXTURE_VIEW_MIN_LAYER), 0);
        EXPECT_EQ(GetViewParameter(texture, GL_TEXTURE_2D, GL_TEXTURE_VIEW_NUM_LAYERS), 0);
    }

    TEST_F(TextureViewTest, TexStorageSeedsTheFullExtentAsViewState) {
        const GLuint texture = MakeImmutable2D(3, 16, 16);
        DrainPendingGlErrors();

        EXPECT_EQ(GetViewParameter(texture, GL_TEXTURE_2D, GL_TEXTURE_VIEW_MIN_LEVEL), 0);
        EXPECT_EQ(GetViewParameter(texture, GL_TEXTURE_2D, GL_TEXTURE_VIEW_NUM_LEVELS), 3);
        EXPECT_EQ(GetViewParameter(texture, GL_TEXTURE_2D, GL_TEXTURE_VIEW_MIN_LAYER), 0);
        EXPECT_EQ(GetViewParameter(texture, GL_TEXTURE_2D, GL_TEXTURE_VIEW_NUM_LAYERS), 1);
    }

    TEST_F(TextureViewTest, TexStorageOnAnArrayReportsItsLayerCount) {
        const GLuint texture = MakeImmutable2DArray(1, 16, 6);
        DrainPendingGlErrors();

        EXPECT_EQ(GetViewParameter(texture, GL_TEXTURE_2D_ARRAY, GL_TEXTURE_VIEW_NUM_LAYERS), 6);
    }

    // ============================ the derived view state ============================

    TEST_F(TextureViewTest, ViewDerivesItsRangeAndInheritsImmutableLevels) {
        const GLuint storage = MakeImmutable2D(3, 16, 16);
        const GLuint view = GenTexture();
        MG_Impl::GLImpl::TextureView(view, GL_TEXTURE_2D, storage, GL_RGBA8, 1, 2, 0, 1);
        ExpectSingleGlError(GL_NO_ERROR);

        EXPECT_EQ(GetViewParameter(view, GL_TEXTURE_2D, GL_TEXTURE_VIEW_MIN_LEVEL), 1);
        EXPECT_EQ(GetViewParameter(view, GL_TEXTURE_2D, GL_TEXTURE_VIEW_NUM_LEVELS), 2);
        EXPECT_EQ(GetViewParameter(view, GL_TEXTURE_2D, GL_TEXTURE_VIEW_MIN_LAYER), 0);
        EXPECT_EQ(GetViewParameter(view, GL_TEXTURE_2D, GL_TEXTURE_VIEW_NUM_LAYERS), 1);
        EXPECT_EQ(GetViewParameter(view, GL_TEXTURE_2D, GL_TEXTURE_IMMUTABLE_FORMAT), GL_TRUE);
        // 8.18: "TEXTURE_IMMUTABLE_LEVELS is set to the value of TEXTURE_IMMUTABLE_LEVELS from
        // the ORIGINAL texture" - not to <numlevels>.
        EXPECT_EQ(GetViewParameter(view, GL_TEXTURE_2D, GL_TEXTURE_IMMUTABLE_LEVELS), 3);
    }

    // ...and the level a FRAMEBUFFER may attach is the view's own count, not the inherited
    // TEXTURE_IMMUTABLE_LEVELS the test above pins. Bounding glFramebufferTexture by the latter
    // accepted a level the view cannot reach, which attaches a 0x0 image: the framebuffer then
    // reports COMPLETE and nothing can be drawn into it.
    TEST_F(TextureViewTest, AFramebufferAttachIsBoundedByTheViewsOwnLevelCount) {
        const GLuint storage = MakeImmutable2D(4, 32, 32);
        const GLuint view = GenTexture();
        MG_Impl::GLImpl::TextureView(view, GL_TEXTURE_2D, storage, GL_RGBA8, /*minlevel=*/2,
                                     /*numlevels=*/2, 0, 1);
        ExpectSingleGlError(GL_NO_ERROR);
        // The inherited query really does report the original's four levels...
        ASSERT_EQ(GetViewParameter(view, GL_TEXTURE_2D, GL_TEXTURE_IMMUTABLE_LEVELS), 4);
        // ...while the view itself has two.
        ASSERT_EQ(GetViewParameter(view, GL_TEXTURE_2D, GL_TEXTURE_VIEW_NUM_LEVELS), 2);

        GLuint framebuffer = 0;
        MG_Impl::GLImpl::CreateFramebuffers(1, &framebuffer);
        MG_Impl::GLImpl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
        ExpectSingleGlError(GL_NO_ERROR);

        MG_Impl::GLImpl::FramebufferTexture(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, view, 1);
        ExpectSingleGlError(GL_NO_ERROR);

        MG_Impl::GLImpl::FramebufferTexture(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, view, 2);
        ExpectSingleGlError(GL_INVALID_VALUE);

        MG_Impl::GLImpl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        DrainPendingGlErrors();
    }

    TEST_F(TextureViewTest, ViewClampsItsLevelCountToWhatRemains) {
        const GLuint storage = MakeImmutable2D(3, 16, 16);
        const GLuint view = GenTexture();
        // 8.18: NUM_LEVELS is "the lesser of <numlevels> and TEXTURE_VIEW_NUM_LEVELS from the
        // original minus <minlevel>", so an over-large request clamps rather than erroring.
        MG_Impl::GLImpl::TextureView(view, GL_TEXTURE_2D, storage, GL_RGBA8, 2, 10, 0, 1);
        ExpectSingleGlError(GL_NO_ERROR);

        EXPECT_EQ(GetViewParameter(view, GL_TEXTURE_2D, GL_TEXTURE_VIEW_MIN_LEVEL), 2);
        EXPECT_EQ(GetViewParameter(view, GL_TEXTURE_2D, GL_TEXTURE_VIEW_NUM_LEVELS), 1);
    }

    TEST_F(TextureViewTest, ViewOfAViewComposesOntoTheOriginalRatherThanRestarting) {
        const GLuint storage = MakeImmutable2D(4, 32, 32);
        const GLuint first = GenTexture();
        MG_Impl::GLImpl::TextureView(first, GL_TEXTURE_2D, storage, GL_RGBA8, 1, 3, 0, 1);
        ExpectSingleGlError(GL_NO_ERROR);
        const GLuint second = GenTexture();
        MG_Impl::GLImpl::TextureView(second, GL_TEXTURE_2D, first, GL_RGBA8, 2, 1, 0, 1);
        ExpectSingleGlError(GL_NO_ERROR);

        // 8.18: MIN_LEVEL is "<minlevel> plus TEXTURE_VIEW_MIN_LEVEL from the original".
        EXPECT_EQ(GetViewParameter(second, GL_TEXTURE_2D, GL_TEXTURE_VIEW_MIN_LEVEL), 3);
        EXPECT_EQ(GetViewParameter(second, GL_TEXTURE_2D, GL_TEXTURE_VIEW_NUM_LEVELS), 1);
        EXPECT_EQ(GetViewParameter(second, GL_TEXTURE_2D, GL_TEXTURE_IMMUTABLE_LEVELS), 4);

        // And the composed view points at the ROOT, not at the intermediate one - which is what
        // lets both backends resolve a view's storage in a single hop.
        const auto& secondObject = MG_State::pGLContext->GetTextureObject(second);
        const auto& storageObject = MG_State::pGLContext->GetTextureObject(storage);
        ASSERT_TRUE(secondObject != nullptr);
        EXPECT_TRUE(secondObject->IsTextureView());
        EXPECT_EQ(secondObject->GetViewStorageOwner().get(), storageObject.get());
    }

    TEST_F(TextureViewTest, ViewCarriesItsOwnParametersAndFormat) {
        const GLuint storage = MakeImmutable2D(1, 16, 16, GL_DEPTH24_STENCIL8);
        MG_Impl::GLImpl::TexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_STENCIL_INDEX);
        const GLuint view = GenTexture();
        MG_Impl::GLImpl::TextureView(view, GL_TEXTURE_2D, storage, GL_DEPTH24_STENCIL8, 0, 1, 0, 1);
        ExpectSingleGlError(GL_NO_ERROR);
        MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, view);
        MG_Impl::GLImpl::TexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_DEPTH_COMPONENT);
        DrainPendingGlErrors();

        // This divergence IS the feature (it is what Better Clouds uses glTextureView for): one
        // storage read through two names with two different aspects in the same shading pass.
        EXPECT_EQ(GetViewParameter(storage, GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE), GL_STENCIL_INDEX);
        EXPECT_EQ(GetViewParameter(view, GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE), GL_DEPTH_COMPONENT);
    }

    TEST_F(TextureViewTest, DeletingTheOriginalLeavesTheViewsStorageAlive) {
        const GLuint storage = MakeImmutable2D(1, 16, 16);
        const GLuint view = GenTexture();
        MG_Impl::GLImpl::TextureView(view, GL_TEXTURE_2D, storage, GL_RGBA8, 0, 1, 0, 1);
        ExpectSingleGlError(GL_NO_ERROR);

        const auto storageObject = MG_State::pGLContext->GetTextureObject(storage);
        ASSERT_TRUE(storageObject != nullptr);
        MG_Impl::GLImpl::DeleteTextures(1, &storage);
        DrainPendingGlErrors();

        // GL 4.6 core 5.1.2: the NAME is gone, the object is not - something still refers to it.
        EXPECT_EQ(MG_Impl::GLImpl::IsTexture(storage), static_cast<GLboolean>(GL_FALSE));
        const auto& viewObject = MG_State::pGLContext->GetTextureObject(view);
        ASSERT_TRUE(viewObject != nullptr);
        EXPECT_EQ(viewObject->GetViewStorageOwner().get(), storageObject.get());
        EXPECT_EQ(GetViewParameter(view, GL_TEXTURE_2D, GL_TEXTURE_IMMUTABLE_FORMAT), GL_TRUE);
    }

    // ============================ the error surface ============================
    // KHR-GL43.texture_view.errors walks these in this order; the letters are its own.

    TEST_F(TextureViewTest, ZeroTextureIsInvalidValue) {  // (a)
        const GLuint storage = MakeImmutable2D();
        DrainPendingGlErrors();
        MG_Impl::GLImpl::TextureView(0, GL_TEXTURE_2D, storage, GL_RGBA8, 0, 1, 0, 1);
        ExpectSingleGlError(GL_INVALID_VALUE);
    }

    TEST_F(TextureViewTest, TextureThatGenTexturesNeverReturnedIsInvalidOperation) {  // (b)
        const GLuint storage = MakeImmutable2D();
        DrainPendingGlErrors();
        MG_Impl::GLImpl::TextureView(0xFFFFFFFFu, GL_TEXTURE_2D, storage, GL_RGBA8, 0, 1, 0, 1);
        ExpectSingleGlError(GL_INVALID_OPERATION);
    }

    TEST_F(TextureViewTest, AlreadyBoundTextureIsInvalidOperation) {  // (c)
        const GLuint storage = MakeImmutable2D();
        const GLuint alreadyBound = GenTexture();
        MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, alreadyBound);
        DrainPendingGlErrors();

        MG_Impl::GLImpl::TextureView(alreadyBound, GL_TEXTURE_2D, storage, GL_RGBA8, 0, 1, 0, 1);
        ExpectSingleGlError(GL_INVALID_OPERATION);
    }

    TEST_F(TextureViewTest, OrigTextureThatIsNotATextureObjectIsInvalidValue) {  // (d)
        const GLuint view = GenTexture();
        DrainPendingGlErrors();
        // Note the code differs from (b): INVALID_VALUE here, INVALID_OPERATION there.
        MG_Impl::GLImpl::TextureView(view, GL_TEXTURE_2D, 0xFFFFFFFFu, GL_RGBA8, 0, 1, 0, 1);
        ExpectSingleGlError(GL_INVALID_VALUE);
    }

    TEST_F(TextureViewTest, MutableOrigTextureIsInvalidOperation) {  // (e)
        const GLuint mutableTexture = GenTexture();
        MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, mutableTexture);
        MG_Impl::GLImpl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        const GLuint view = GenTexture();
        DrainPendingGlErrors();

        MG_Impl::GLImpl::TextureView(view, GL_TEXTURE_2D, mutableTexture, GL_RGBA8, 0, 1, 0, 1);
        ExpectSingleGlError(GL_INVALID_OPERATION);
    }

    TEST_F(TextureViewTest, IncompatibleTargetPairIsInvalidOperation) {  // (f)
        const GLuint storage = MakeImmutable2D();
        const GLuint view = GenTexture();
        DrainPendingGlErrors();
        // Table 8.20 admits 2D -> 2D and 2D -> 2D_ARRAY, and nothing else.
        MG_Impl::GLImpl::TextureView(view, GL_TEXTURE_3D, storage, GL_RGBA8, 0, 1, 0, 1);
        ExpectSingleGlError(GL_INVALID_OPERATION);
    }

    TEST_F(TextureViewTest, LegalTargetPairsAreAccepted) {  // (f), the other way round
        const GLuint storage = MakeImmutable2D(1, 16, 16);
        const GLuint sameTarget = GenTexture();
        MG_Impl::GLImpl::TextureView(sameTarget, GL_TEXTURE_2D, storage, GL_RGBA8, 0, 1, 0, 1);
        ExpectSingleGlError(GL_NO_ERROR);
        const GLuint arrayView = GenTexture();
        MG_Impl::GLImpl::TextureView(arrayView, GL_TEXTURE_2D_ARRAY, storage, GL_RGBA8, 0, 1, 0, 1);
        ExpectSingleGlError(GL_NO_ERROR);
    }

    TEST_F(TextureViewTest, FormatFromAnotherViewClassIsInvalidOperation) {  // (g)
        const GLuint storage = MakeImmutable2D(1, 16, 16, GL_RGBA8);
        const GLuint view = GenTexture();
        DrainPendingGlErrors();
        // GL_RGBA8 is VIEW_CLASS_32_BITS; GL_R8 is VIEW_CLASS_8_BITS.
        MG_Impl::GLImpl::TextureView(view, GL_TEXTURE_2D, storage, GL_R8, 0, 1, 0, 1);
        ExpectSingleGlError(GL_INVALID_OPERATION);
    }

    TEST_F(TextureViewTest, FormatFromTheSameViewClassIsAccepted) {  // (g), the other way round
        const GLuint storage = MakeImmutable2D(1, 16, 16, GL_RGBA8);
        const GLuint view = GenTexture();
        // Both VIEW_CLASS_32_BITS.
        MG_Impl::GLImpl::TextureView(view, GL_TEXTURE_2D, storage, GL_R32UI, 0, 1, 0, 1);
        ExpectSingleGlError(GL_NO_ERROR);

        GLint internalFormat = 0;
        MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, view);
        MG_Impl::GLImpl::GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &internalFormat);
        DrainPendingGlErrors();
        EXPECT_EQ(internalFormat, GL_R32UI) << "the view must take the format it was asked for, not its parent's";
    }

    TEST_F(TextureViewTest, ClasslessFormatMayOnlyBeViewedAsItself) {  // (h)
        // Depth, stencil and depth/stencil formats have NO entry in table 8.21, which the spec
        // turns into a stricter rule than "same class": the view's format must be IDENTICAL.
        // This is the rule the Better Clouds D24S8 view depends on being permissive enough.
        const GLuint storage = MakeImmutable2D(1, 16, 16, GL_DEPTH24_STENCIL8);
        const GLuint sameFormat = GenTexture();
        MG_Impl::GLImpl::TextureView(sameFormat, GL_TEXTURE_2D, storage, GL_DEPTH24_STENCIL8, 0, 1, 0, 1);
        ExpectSingleGlError(GL_NO_ERROR);

        const GLuint otherFormat = GenTexture();
        MG_Impl::GLImpl::TextureView(otherFormat, GL_TEXTURE_2D, storage, GL_RGBA8, 0, 1, 0, 1);
        ExpectSingleGlError(GL_INVALID_OPERATION);
    }

    TEST_F(TextureViewTest, MinLevelPastTheLastLevelIsInvalidValue) {  // (i)
        const GLuint storage = MakeImmutable2D(2, 16, 16);
        const GLuint view = GenTexture();
        DrainPendingGlErrors();
        MG_Impl::GLImpl::TextureView(view, GL_TEXTURE_2D, storage, GL_RGBA8, 2, 1, 0, 1);
        ExpectSingleGlError(GL_INVALID_VALUE);
    }

    TEST_F(TextureViewTest, MinLayerPastTheLastLayerIsInvalidValue) {  // (j)
        const GLuint storage = MakeImmutable2DArray(1, 16, 4);
        const GLuint view = GenTexture();
        DrainPendingGlErrors();
        MG_Impl::GLImpl::TextureView(view, GL_TEXTURE_2D, storage, GL_RGBA8, 0, 1, 4, 1);
        ExpectSingleGlError(GL_INVALID_VALUE);
    }

    TEST_F(TextureViewTest, CubeMapViewDemandsExactlySixLayers) {  // (k)
        const GLuint storage = MakeImmutable2DArray(1, 16, 6);
        const GLuint view = GenTexture();
        DrainPendingGlErrors();
        MG_Impl::GLImpl::TextureView(view, GL_TEXTURE_CUBE_MAP, storage, GL_RGBA8, 0, 1, 0, 5);
        ExpectSingleGlError(GL_INVALID_VALUE);
    }

    TEST_F(TextureViewTest, CubeMapArrayViewDemandsAMultipleOfSixLayers) {  // (l)
        const GLuint storage = MakeImmutable2DArray(1, 16, 12);
        const GLuint view = GenTexture();
        DrainPendingGlErrors();
        MG_Impl::GLImpl::TextureView(view, GL_TEXTURE_CUBE_MAP_ARRAY, storage, GL_RGBA8, 0, 1, 0, 1);
        ExpectSingleGlError(GL_INVALID_VALUE);
    }

    TEST_F(TextureViewTest, SingleLayerTargetsRejectMoreThanOneLayer) {  // (m).. (q)
        const GLuint storage = MakeImmutable2DArray(1, 16, 4);
        DrainPendingGlErrors();
        for (const GLenum target : {GL_TEXTURE_2D}) {
            const GLuint view = GenTexture();
            MG_Impl::GLImpl::TextureView(view, target, storage, GL_RGBA8, 0, 1, 0, 2);
            ExpectSingleGlError(GL_INVALID_VALUE);
        }
        // The 1D and 3D forms take the same rule; check one of them from a legal parent so the
        // target-pair rule cannot be what is rejecting it.
        const GLuint texture3D = GenTexture();
        MG_Impl::GLImpl::BindTexture(GL_TEXTURE_3D, texture3D);
        MG_Impl::GLImpl::TexStorage3D(GL_TEXTURE_3D, 1, GL_RGBA8, 8, 8, 8);
        DrainPendingGlErrors();
        const GLuint view3D = GenTexture();
        MG_Impl::GLImpl::TextureView(view3D, GL_TEXTURE_3D, texture3D, GL_RGBA8, 0, 1, 0, 2);
        ExpectSingleGlError(GL_INVALID_VALUE);
    }

    TEST_F(TextureViewTest, CubeMapViewDemandsSquareLevels) {  // (r)
        const GLuint storage = GenTexture();
        MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D_ARRAY, storage);
        MG_Impl::GLImpl::TexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, 32, 33, 6);
        DrainPendingGlErrors();

        const GLuint view = GenTexture();
        MG_Impl::GLImpl::TextureView(view, GL_TEXTURE_CUBE_MAP, storage, GL_RGBA8, 0, 1, 0, 6);
        ExpectSingleGlError(GL_INVALID_OPERATION);
    }

    TEST_F(TextureViewTest, BufferTextureHasNoLegalViewTarget) {
        // Table 8.20 lists nothing for GL_TEXTURE_BUFFER: its storage is a buffer object, so
        // there is no image to view.
        const GLuint storage = MakeImmutable2D();
        const GLuint view = GenTexture();
        DrainPendingGlErrors();
        MG_Impl::GLImpl::TextureView(view, GL_TEXTURE_BUFFER, storage, GL_RGBA8, 0, 1, 0, 1);
        ExpectSingleGlError(GL_INVALID_OPERATION);
    }

    TEST_F(TextureViewTest, NonTextureTargetIsInvalidEnum) {
        const GLuint storage = MakeImmutable2D();
        const GLuint view = GenTexture();
        DrainPendingGlErrors();
        MG_Impl::GLImpl::TextureView(view, GL_ARRAY_BUFFER, storage, GL_RGBA8, 0, 1, 0, 1);
        ExpectSingleGlError(GL_INVALID_ENUM);
    }

    TEST_F(TextureViewTest, AFailedCallLeavesTheNameUninstantiated) {
        // The spec's "texture must not already have a target" rule means a rejected call has to
        // leave the name exactly as GenTextures left it, or a retry would then fail with (c).
        const GLuint storage = MakeImmutable2D();
        const GLuint view = GenTexture();
        DrainPendingGlErrors();
        MG_Impl::GLImpl::TextureView(view, GL_TEXTURE_3D, storage, GL_RGBA8, 0, 1, 0, 1);
        ExpectSingleGlError(GL_INVALID_OPERATION);
        EXPECT_FALSE(MG_State::pGLContext->ValidateTextureObject(view));

        MG_Impl::GLImpl::TextureView(view, GL_TEXTURE_2D, storage, GL_RGBA8, 0, 1, 0, 1);
        ExpectSingleGlError(GL_NO_ERROR);
        EXPECT_TRUE(MG_State::pGLContext->ValidateTextureObject(view));
    }

    // ============================ the no-support contract ============================

    TEST_F(TextureViewTest, BackendWithoutTextureViewSupportRaisesInvalidOperation) {
        const GLuint storage = MakeImmutable2D();
        DrainPendingGlErrors();

        // A backend that cannot give two texture names one storage - ES without
        // EXT/OES_texture_view - withholds GL_ARB_texture_view, and glTextureView must then FAIL
        // rather than quietly produce a view with no storage. A silent no-op is the one
        // unacceptable answer: it is indistinguishable from success at the call site, and the
        // application renders from a texture that aliases nothing.
        const ScopedBackendOverride noTextureView(false);
        const GLuint view = GenTexture();
        MG_Impl::GLImpl::TextureView(view, GL_TEXTURE_2D, storage, GL_RGBA8, 0, 1, 0, 1);
        ExpectSingleGlError(GL_INVALID_OPERATION);
        EXPECT_FALSE(MG_State::pGLContext->ValidateTextureObject(view));
    }

    // ======================= which of the owner's layers a face names =======================

    // A GL_TEXTURE_CUBE_MAP view over a LAYERED owner - a 2D array here, a cube-map ARRAY behaves
    // identically - is the one shape where the face a target names cannot be carried by the choice
    // of blob: the owner keeps every layer in ONE blob, so there is nothing for
    // ToOwnerUploadTarget to choose between and the face has to land in the byte offset instead.
    // It did not. The offset shifted by the view's layer origin alone, so all six face tokens read
    // the view's FIRST layer-face - silently, with real texels from a real layer, on every path
    // that answers out of the CPU shadow.
    //
    // The shadow is exactly what this exercises: the fixture's backend is not DirectVulkan, so the
    // by-name readback takes the shadow arm rather than asking a backend. (DirectVulkan's own path
    // resolves the face into a Vulkan baseArrayLayer and was always right, which is what made this
    // a disagreement between the two backends rather than a uniform wrong answer.)
    TEST_F(TextureViewTest, CubeMapViewOfAnArrayReadsTheFaceEachTokenNames) {
        constexpr GLint kLayers = 8;
        constexpr GLint kViewMinLayer = 2;

        const GLuint storage = MakeImmutable2DArray(1, 1, kLayers);
        // Every layer carries its own index, so a read that lands on the wrong one says which one
        // answered instead of merely failing.
        for (GLint layer = 0; layer < kLayers; ++layer) {
            const Uint8 texel[] = {static_cast<Uint8>(10 + layer), 20, 30, 40};
            MG_Impl::GLImpl::TexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                           texel);
        }
        DrainPendingGlErrors();

        const GLuint view = GenTexture();
        MG_Impl::GLImpl::TextureView(view, GL_TEXTURE_CUBE_MAP, storage, GL_RGBA8, 0, 1, kViewMinLayer, 6);
        ASSERT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR) << "the cube-map view over the array was refused";

        for (GLint face = 0; face < 6; ++face) {
            Uint8 output[4] = {};
            MG_Impl::GLImpl::GetTextureSubImage(view, 0, 0, 0, face, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                                sizeof(output), output);
            EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR) << "reading face " << face << " errored";
            EXPECT_EQ(static_cast<GLint>(output[0]), 10 + kViewMinLayer + face)
                << "face " << face << " of a view based at layer " << kViewMinLayer << " answered with layer "
                << (static_cast<GLint>(output[0]) - 10);
        }
    }
} // namespace
