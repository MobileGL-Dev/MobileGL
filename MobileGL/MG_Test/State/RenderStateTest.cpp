// MobileGL - MobileGL/MG_Test/State/RenderStateTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Indexed capability state (glEnablei/glDisablei/glIsEnabledi) exists only for GL_BLEND in this
// stack. Every other capability must come back as GL_INVALID_ENUM per GL 4.6 sec. 17.3.3 - and,
// far more importantly, must come back at all: RenderState::SetCapabilityIndexed and
// IsCapabilityEnabledIndexed used to answer a non-blend capability with THROW_UNIMPL_EXCEPTION,
// which unwinds a C++ exception through the C GL ABI and terminates the process.

#include <gtest/gtest.h>

#include "Includes.h"
#include "Init.h"

#include <MG_Impl/GLImpl/Getter/GL_Getter.h>
#include <MG_Impl/GLImpl/RenderState/GL_RenderState.h>
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/FramebufferState/FramebufferObject.h>

using namespace MobileGL;

namespace {
    class RenderStateTest: public ::testing::Test {
    protected:
        // GL error flags are sticky per code and the context outlives an individual test in this
        // binary, so a pending error from an earlier case would be handed to the next GetError().
        static void DrainPendingGlErrors() {
            for (Int drained = 0; drained < 16 && MG_Impl::GLImpl::GetError() != GL_NO_ERROR; ++drained) {
            }
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
} // namespace

TEST_F(RenderStateTest, IndexedCapabilityTogglesRejectNonBlendCapabilities) {
    // GL_CLIP_DISTANCE0 is a real capability, just not an indexed one - the shape an application or
    // a CTS negative test would hit.
    for (const GLenum cap : {GL_CLIP_DISTANCE0, GL_DEPTH_TEST, GL_SCISSOR_TEST}) {
        MG_Impl::GLImpl::Enablei(cap, 0);
        ExpectSingleGlError(GL_INVALID_ENUM);

        MG_Impl::GLImpl::Disablei(cap, 0);
        ExpectSingleGlError(GL_INVALID_ENUM);

        EXPECT_EQ(MG_Impl::GLImpl::IsEnabledi(cap, 0), GL_FALSE);
        ExpectSingleGlError(GL_INVALID_ENUM);
    }
}

TEST_F(RenderStateTest, IndexedCapabilityTogglesRejectAnOutOfRangeBufferIndex) {
    const GLuint outOfRange = MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS;

    MG_Impl::GLImpl::Enablei(GL_BLEND, outOfRange);
    ExpectSingleGlError(GL_INVALID_VALUE);

    MG_Impl::GLImpl::Disablei(GL_BLEND, outOfRange);
    ExpectSingleGlError(GL_INVALID_VALUE);

    EXPECT_EQ(MG_Impl::GLImpl::IsEnabledi(GL_BLEND, outOfRange), GL_FALSE);
    ExpectSingleGlError(GL_INVALID_VALUE);
}

TEST_F(RenderStateTest, IndexedBlendTogglesStillWork) {
    // The rejection path must not have cost the one capability that is genuinely indexed.
    MG_Impl::GLImpl::Enablei(GL_BLEND, 1);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_EQ(MG_Impl::GLImpl::IsEnabledi(GL_BLEND, 1), GL_TRUE);

    MG_Impl::GLImpl::Disablei(GL_BLEND, 1);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
    EXPECT_EQ(MG_Impl::GLImpl::IsEnabledi(GL_BLEND, 1), GL_FALSE);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);
}
