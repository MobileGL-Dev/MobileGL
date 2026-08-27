// MobileGL - MobileGL/MG_Test/State/RenderStateTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Indexed capability state (glEnablei/glDisablei/glIsEnabledi) exists for exactly two
// capabilities: GL_BLEND, indexed by draw buffer, and GL_SCISSOR_TEST, indexed by viewport
// (ARB_viewport_array). Every other capability must come back as GL_INVALID_ENUM per GL 4.6
// sec. 17.3.3 - and, far more importantly, must come back at all: RenderState::SetCapabilityIndexed
// and IsCapabilityEnabledIndexed used to answer a non-blend capability with THROW_UNIMPL_EXCEPTION,
// which unwinds a C++ exception through the C GL ABI and terminates the process.
//
// The second half of this file is the ARB_viewport_array indexed rectangle state. Every one of
// glViewportArrayv/glViewportIndexedf(v)/glScissorArrayv/glScissorIndexed(v)/glDepthRangeArrayv/
// glDepthRangeIndexed was a MGLOG_W_ONCE stub that raised no error and stored nothing, and the
// indexed getters answered EVERY index with viewport 0's value, so a set/get round trip silently
// reported the initial state. The assertions below are deliberately state-shaped rather than
// render-shaped: this IS the state machine, and the rendering half (gl_ViewportIndex routing) is
// asserted separately in MG_IntegrationTest/Scenarios/ViewportArrayScenario.cpp.

#include <gtest/gtest.h>

#include "Includes.h"
#include "Init.h"

#include <limits>

#include <MG_Impl/GLImpl/Drawing/GL_Drawing.h>
#include <MG_Impl/GLImpl/Getter/GL_Getter.h>
#include <MG_Impl/GLImpl/RenderState/GL_RenderState.h>
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/FramebufferState/FramebufferObject.h>
#include <MG_State/GLState/RenderState/RenderState.h>

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

TEST_F(RenderStateTest, IndexedCapabilityTogglesRejectNonIndexedCapabilities) {
    // GL_CLIP_DISTANCE0 is a real capability, just not an indexed one - the shape an application or
    // a CTS negative test would hit. GL_SCISSOR_TEST used to be in this list and is not any more:
    // ARB_viewport_array makes it the second indexed capability (see the tests below).
    for (const GLenum cap : {GL_CLIP_DISTANCE0, GL_DEPTH_TEST, GL_STENCIL_TEST}) {
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

// ---------------------------------------------------------------------------------------------
// ARB_viewport_array: indexed viewport / scissor / depth-range state
// ---------------------------------------------------------------------------------------------

namespace {
    constexpr GLuint kMaxViewports = RenderStateParameters::MAX_VIEWPORTS;

    Array<Array<GLfloat, 4>, kMaxViewports> ReadAllViewports() {
        Array<Array<GLfloat, 4>, kMaxViewports> out{};
        for (GLuint i = 0; i < kMaxViewports; ++i) {
            MG_Impl::GLImpl::GetFloati_v(GL_VIEWPORT, i, out[i].data());
        }
        return out;
    }

    Array<Array<GLdouble, 2>, kMaxViewports> ReadAllDepthRanges() {
        Array<Array<GLdouble, 2>, kMaxViewports> out{};
        for (GLuint i = 0; i < kMaxViewports; ++i) {
            MG_Impl::GLImpl::GetDoublei_v(GL_DEPTH_RANGE, i, out[i].data());
        }
        return out;
    }
} // namespace

TEST_F(RenderStateTest, ScissorTestIsIndexedByViewport) {
    // The exact shape of KHR-GL43.viewport_array.scissor_test_state_api's toggle loop: one index
    // is flipped and EVERY index is read back, so a broadcast masquerading as an indexed write
    // cannot pass.
    MG_Impl::GLImpl::Disable(GL_SCISSOR_TEST);
    ExpectSingleGlError(GL_NO_ERROR);

    for (GLuint toggled = 0; toggled < kMaxViewports; ++toggled) {
        MG_Impl::GLImpl::Enablei(GL_SCISSOR_TEST, toggled);
        EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR) << "index " << toggled;
        for (GLuint i = 0; i < kMaxViewports; ++i) {
            EXPECT_EQ(MG_Impl::GLImpl::IsEnabledi(GL_SCISSOR_TEST, i), i == toggled ? GL_TRUE : GL_FALSE)
                << "enabled index " << toggled << ", read index " << i;
        }
        MG_Impl::GLImpl::Disablei(GL_SCISSOR_TEST, toggled);
        EXPECT_EQ(MG_Impl::GLImpl::IsEnabledi(GL_SCISSOR_TEST, toggled), GL_FALSE);
    }
    ExpectSingleGlError(GL_NO_ERROR);
}

TEST_F(RenderStateTest, NonIndexedScissorTestEnableWritesEveryViewport) {
    // GL 4.6 core 17.3.2: Enable/Disable(SCISSOR_TEST) is "for all viewports". Reading only
    // index 0 back would let a broadcast-less implementation through, so every index is checked.
    MG_Impl::GLImpl::Enable(GL_SCISSOR_TEST);
    for (GLuint i = 0; i < kMaxViewports; ++i) {
        EXPECT_EQ(MG_Impl::GLImpl::IsEnabledi(GL_SCISSOR_TEST, i), GL_TRUE) << "index " << i;
    }
    // ... and the non-indexed query answers for viewport 0 (GL 4.6 core 22.1).
    EXPECT_EQ(MG_Impl::GLImpl::IsEnabled(GL_SCISSOR_TEST), GL_TRUE);

    MG_Impl::GLImpl::Disable(GL_SCISSOR_TEST);
    for (GLuint i = 0; i < kMaxViewports; ++i) {
        EXPECT_EQ(MG_Impl::GLImpl::IsEnabledi(GL_SCISSOR_TEST, i), GL_FALSE) << "index " << i;
    }
    EXPECT_EQ(MG_Impl::GLImpl::IsEnabled(GL_SCISSOR_TEST), GL_FALSE);

    // An indexed enable on a NON-zero index must not move the non-indexed answer.
    MG_Impl::GLImpl::Enablei(GL_SCISSOR_TEST, 3);
    EXPECT_EQ(MG_Impl::GLImpl::IsEnabled(GL_SCISSOR_TEST), GL_FALSE);
    MG_Impl::GLImpl::Enablei(GL_SCISSOR_TEST, 0);
    EXPECT_EQ(MG_Impl::GLImpl::IsEnabled(GL_SCISSOR_TEST), GL_TRUE);

    MG_Impl::GLImpl::Disable(GL_SCISSOR_TEST);
    ExpectSingleGlError(GL_NO_ERROR);
}

TEST_F(RenderStateTest, ScissorTestEnableRejectsAnOutOfRangeViewportIndex) {
    MG_Impl::GLImpl::Enablei(GL_SCISSOR_TEST, kMaxViewports);
    ExpectSingleGlError(GL_INVALID_VALUE);

    MG_Impl::GLImpl::Disablei(GL_SCISSOR_TEST, kMaxViewports);
    ExpectSingleGlError(GL_INVALID_VALUE);

    EXPECT_EQ(MG_Impl::GLImpl::IsEnabledi(GL_SCISSOR_TEST, kMaxViewports), GL_FALSE);
    ExpectSingleGlError(GL_INVALID_VALUE);

    // MAX_VIEWPORTS - 1 is the last LEGAL index and must stay silent.
    MG_Impl::GLImpl::Enablei(GL_SCISSOR_TEST, kMaxViewports - 1);
    ExpectSingleGlError(GL_NO_ERROR);
    MG_Impl::GLImpl::Disablei(GL_SCISSOR_TEST, kMaxViewports - 1);
    ExpectSingleGlError(GL_NO_ERROR);
}

TEST_F(RenderStateTest, MaxViewportsMatchesTheIndexedStateWidth) {
    // The advertised limit and the width of the state arrays are the same number by
    // construction; a divergence would make some index simultaneously legal to the CTS and
    // out of range to the setters.
    GLint maxViewports = 0;
    MG_Impl::GLImpl::GetIntegerv(GL_MAX_VIEWPORTS, &maxViewports);
    ExpectSingleGlError(GL_NO_ERROR);
    EXPECT_EQ(maxViewports, static_cast<GLint>(kMaxViewports));
    EXPECT_GE(maxViewports, 16) << "GL 4.3 core requires MAX_VIEWPORTS >= 16";
}

TEST_F(RenderStateTest, ViewportArrayvRoundTripsThroughEveryGetterWidth) {
    Array<GLfloat, kMaxViewports * 4> written{};
    for (GLuint i = 0; i < kMaxViewports; ++i) {
        written[i * 4 + 0] = static_cast<GLfloat>(i) + 0.125f;
        written[i * 4 + 1] = static_cast<GLfloat>(i) + 0.25f;
        written[i * 4 + 2] = static_cast<GLfloat>(64 + i);
        written[i * 4 + 3] = static_cast<GLfloat>(32 + i);
    }
    MG_Impl::GLImpl::ViewportArrayv(0, kMaxViewports, written.data());
    ExpectSingleGlError(GL_NO_ERROR);

    for (GLuint i = 0; i < kMaxViewports; ++i) {
        GLfloat asFloat[4] = {};
        MG_Impl::GLImpl::GetFloati_v(GL_VIEWPORT, i, asFloat);
        // Bit-exact: the fractional origin is the whole point of float viewport state, and the
        // CTS compares with == (0.125 and 0.25 are exact binary fractions, so this is fair).
        EXPECT_EQ(asFloat[0], written[i * 4 + 0]) << "index " << i << " must round-trip verbatim";
        EXPECT_EQ(asFloat[1], written[i * 4 + 1]) << "index " << i;
        EXPECT_EQ(asFloat[2], written[i * 4 + 2]) << "index " << i;
        EXPECT_EQ(asFloat[3], written[i * 4 + 3]) << "index " << i;

        GLdouble asDouble[4] = {};
        MG_Impl::GLImpl::GetDoublei_v(GL_VIEWPORT, i, asDouble);
        for (int c = 0; c < 4; ++c) {
            EXPECT_EQ(asDouble[c], static_cast<GLdouble>(written[i * 4 + c])) << "index " << i << " component " << c;
        }

        // The integer widths round to nearest rather than truncate; the .5+ case is pinned by
        // ViewportRoundsRatherThanTruncatesForIntegerQueries below.
        GLint asInt[4] = {};
        MG_Impl::GLImpl::GetIntegeri_v(GL_VIEWPORT, i, asInt);
        EXPECT_EQ(asInt[2], static_cast<GLint>(64 + i)) << "index " << i;
        EXPECT_EQ(asInt[3], static_cast<GLint>(32 + i)) << "index " << i;

        GLint64 asInt64[4] = {};
        MG_Impl::GLImpl::GetInteger64i_v(GL_VIEWPORT, i, asInt64);
        for (int c = 0; c < 4; ++c) {
            EXPECT_EQ(asInt64[c], static_cast<GLint64>(asInt[c])) << "index " << i << " component " << c;
        }

        GLboolean asBool[4] = {};
        MG_Impl::GLImpl::GetBooleani_v(GL_VIEWPORT, i, asBool);
        EXPECT_EQ(asBool[2], GL_TRUE) << "index " << i << ": a non-zero width is GL_TRUE";
    }
    ExpectSingleGlError(GL_NO_ERROR);
}

TEST_F(RenderStateTest, ViewportRoundsRatherThanTruncatesForIntegerQueries) {
    MG_Impl::GLImpl::ViewportIndexedf(2, 0.0f, 0.0f, 255.875f, 63.5f);
    ExpectSingleGlError(GL_NO_ERROR);

    GLint asInt[4] = {};
    MG_Impl::GLImpl::GetIntegeri_v(GL_VIEWPORT, 2, asInt);
    EXPECT_EQ(asInt[2], 256);
    EXPECT_EQ(asInt[3], 64);

    GLfloat asFloat[4] = {};
    MG_Impl::GLImpl::GetFloati_v(GL_VIEWPORT, 2, asFloat);
    EXPECT_EQ(asFloat[2], 255.875f) << "the integer query must not disturb the stored float";
    ExpectSingleGlError(GL_NO_ERROR);
}

TEST_F(RenderStateTest, ViewportIndexedWritesTouchExactlyOneIndex) {
    MG_Impl::GLImpl::Viewport(0, 0, 8, 8);
    const auto before = ReadAllViewports();

    for (GLuint target = 0; target < kMaxViewports; ++target) {
        const GLfloat value[4] = {0.375f, 0.375f, 0.625f, 0.625f};
        // Alternate the two indexed entry points so both are covered by the isolation claim.
        if (target % 2 == 0) {
            MG_Impl::GLImpl::ViewportIndexedf(target, value[0], value[1], value[2], value[3]);
        } else {
            MG_Impl::GLImpl::ViewportIndexedfv(target, value);
        }
        ExpectSingleGlError(GL_NO_ERROR);

        const auto after = ReadAllViewports();
        for (GLuint i = 0; i < kMaxViewports; ++i) {
            if (i == target) {
                EXPECT_EQ(after[i][0], value[0]) << "index " << i;
                EXPECT_EQ(after[i][2], value[2]) << "index " << i;
            } else {
                EXPECT_EQ(after[i], before[i]) << "write to " << target << " disturbed index " << i;
            }
        }
        MG_Impl::GLImpl::ViewportIndexedf(target, before[target][0], before[target][1], before[target][2],
                                          before[target][3]);
    }
    ExpectSingleGlError(GL_NO_ERROR);
}

TEST_F(RenderStateTest, ClassicViewportWritesEveryIndexAndIsVisibleThroughIndexZero) {
    // Both directions of the aliasing. ARB_viewport_array defines glViewport as ViewportIndexedf
    // on every index, and glGetIntegerv(GL_VIEWPORT) as viewport 0.
    MG_Impl::GLImpl::ViewportIndexedf(5, 1.0f, 2.0f, 3.0f, 4.0f);
    MG_Impl::GLImpl::Viewport(0, 0, 1, 1);
    ExpectSingleGlError(GL_NO_ERROR);
    for (GLuint i = 0; i < kMaxViewports; ++i) {
        GLfloat data[4] = {};
        MG_Impl::GLImpl::GetFloati_v(GL_VIEWPORT, i, data);
        EXPECT_EQ(data[0], 0.0f) << "index " << i;
        EXPECT_EQ(data[2], 1.0f) << "glViewport must overwrite index " << i;
    }

    MG_Impl::GLImpl::ViewportIndexedf(0, 4.0f, 5.0f, 6.0f, 7.0f);
    GLint classic[4] = {};
    MG_Impl::GLImpl::GetIntegerv(GL_VIEWPORT, classic);
    EXPECT_EQ(classic[0], 4);
    EXPECT_EQ(classic[2], 6);
    GLfloat classicFloat[4] = {};
    MG_Impl::GLImpl::GetFloatv(GL_VIEWPORT, classicFloat);
    EXPECT_EQ(classicFloat[2], 6.0f);
    // Index 5 keeps its own value: writing index 0 is not a broadcast.
    GLfloat other[4] = {};
    MG_Impl::GLImpl::GetFloati_v(GL_VIEWPORT, 5, other);
    EXPECT_EQ(other[2], 1.0f);
    ExpectSingleGlError(GL_NO_ERROR);
}

TEST_F(RenderStateTest, ScissorBoxRoundTripsPerIndexAndAliasesIndexZero) {
    Array<GLint, kMaxViewports * 4> written{};
    for (GLuint i = 0; i < kMaxViewports; ++i) {
        written[i * 4 + 0] = static_cast<GLint>(i);
        written[i * 4 + 1] = static_cast<GLint>(i * 2);
        written[i * 4 + 2] = static_cast<GLint>(16 + i);
        written[i * 4 + 3] = static_cast<GLint>(8 + i);
    }
    MG_Impl::GLImpl::ScissorArrayv(0, kMaxViewports, written.data());
    ExpectSingleGlError(GL_NO_ERROR);

    for (GLuint i = 0; i < kMaxViewports; ++i) {
        GLint readBack[4] = {};
        MG_Impl::GLImpl::GetIntegeri_v(GL_SCISSOR_BOX, i, readBack);
        for (int c = 0; c < 4; ++c) {
            EXPECT_EQ(readBack[c], written[i * 4 + c]) << "index " << i << " component " << c;
        }
    }

    // Indexed writes stay indexed; both spellings.
    MG_Impl::GLImpl::ScissorIndexed(4, 4, 4, 8, 8);
    const GLint indexedV[4] = {9, 9, 12, 12};
    MG_Impl::GLImpl::ScissorIndexedv(7, indexedV);
    ExpectSingleGlError(GL_NO_ERROR);
    GLint probe[4] = {};
    MG_Impl::GLImpl::GetIntegeri_v(GL_SCISSOR_BOX, 4, probe);
    EXPECT_EQ(probe[2], 8);
    MG_Impl::GLImpl::GetIntegeri_v(GL_SCISSOR_BOX, 7, probe);
    EXPECT_EQ(probe[2], 12);
    MG_Impl::GLImpl::GetIntegeri_v(GL_SCISSOR_BOX, 5, probe);
    EXPECT_EQ(probe[2], static_cast<GLint>(16 + 5)) << "index 5 must be untouched";

    // glScissor writes every rectangle, and glGetIntegerv(GL_SCISSOR_BOX) reports rectangle 0.
    MG_Impl::GLImpl::Scissor(2, 3, 5, 6);
    for (GLuint i = 0; i < kMaxViewports; ++i) {
        MG_Impl::GLImpl::GetIntegeri_v(GL_SCISSOR_BOX, i, probe);
        EXPECT_EQ(probe[0], 2) << "index " << i;
        EXPECT_EQ(probe[2], 5) << "index " << i;
    }
    GLint classic[4] = {};
    MG_Impl::GLImpl::GetIntegerv(GL_SCISSOR_BOX, classic);
    EXPECT_EQ(classic[2], 5);
    ExpectSingleGlError(GL_NO_ERROR);
}

TEST_F(RenderStateTest, DepthRangeRoundTripsPerIndexAndAliasesIndexZero) {
    Array<GLdouble, kMaxViewports * 2> written{};
    for (GLuint i = 0; i < kMaxViewports; ++i) {
        // Exact binary fractions, like the CTS uses: a float-backed store round-trips them.
        written[i * 2 + 0] = static_cast<GLdouble>(i) / 16.0;
        written[i * 2 + 1] = 1.0 - static_cast<GLdouble>(i) / 16.0;
    }
    MG_Impl::GLImpl::DepthRangeArrayv(0, kMaxViewports, written.data());
    ExpectSingleGlError(GL_NO_ERROR);

    const auto readBack = ReadAllDepthRanges();
    for (GLuint i = 0; i < kMaxViewports; ++i) {
        EXPECT_EQ(readBack[i][0], written[i * 2 + 0]) << "index " << i;
        EXPECT_EQ(readBack[i][1], written[i * 2 + 1]) << "index " << i;
    }

    MG_Impl::GLImpl::DepthRangeIndexed(9, 0.25, 0.75);
    ExpectSingleGlError(GL_NO_ERROR);
    GLdouble probe[2] = {};
    MG_Impl::GLImpl::GetDoublei_v(GL_DEPTH_RANGE, 9, probe);
    EXPECT_EQ(probe[0], 0.25);
    EXPECT_EQ(probe[1], 0.75);
    MG_Impl::GLImpl::GetDoublei_v(GL_DEPTH_RANGE, 8, probe);
    EXPECT_EQ(probe[0], 8.0 / 16.0) << "index 8 must be untouched";

    GLfloat asFloat[2] = {};
    MG_Impl::GLImpl::GetFloati_v(GL_DEPTH_RANGE, 9, asFloat);
    EXPECT_EQ(asFloat[0], 0.25f);
    EXPECT_EQ(asFloat[1], 0.75f);

    // glDepthRange writes every range; glGetDoublev(GL_DEPTH_RANGE) reports range 0.
    MG_Impl::GLImpl::DepthRange(0.0, 1.0);
    for (GLuint i = 0; i < kMaxViewports; ++i) {
        MG_Impl::GLImpl::GetDoublei_v(GL_DEPTH_RANGE, i, probe);
        EXPECT_EQ(probe[0], 0.0) << "index " << i;
        EXPECT_EQ(probe[1], 1.0) << "index " << i;
    }
    MG_Impl::GLImpl::DepthRangeIndexed(0, 0.125, 0.875);
    GLdouble classic[2] = {};
    MG_Impl::GLImpl::GetDoublev(GL_DEPTH_RANGE, classic);
    EXPECT_EQ(classic[0], 0.125);
    EXPECT_EQ(classic[1], 0.875);
    MG_Impl::GLImpl::DepthRange(0.0, 1.0);
    ExpectSingleGlError(GL_NO_ERROR);
}

TEST_F(RenderStateTest, IndexedRectangleSettersRejectAnOutOfRangeIndex) {
    const GLfloat viewport[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    const GLint scissor[4] = {0, 0, 1, 1};

    for (const GLuint index : {kMaxViewports, kMaxViewports + 1}) {
        MG_Impl::GLImpl::ViewportIndexedf(index, 0.0f, 0.0f, 1.0f, 1.0f);
        ExpectSingleGlError(GL_INVALID_VALUE);
        MG_Impl::GLImpl::ViewportIndexedfv(index, viewport);
        ExpectSingleGlError(GL_INVALID_VALUE);
        MG_Impl::GLImpl::ScissorIndexed(index, 0, 0, 1, 1);
        ExpectSingleGlError(GL_INVALID_VALUE);
        MG_Impl::GLImpl::ScissorIndexedv(index, scissor);
        ExpectSingleGlError(GL_INVALID_VALUE);
        MG_Impl::GLImpl::DepthRangeIndexed(index, 0.0, 1.0);
        ExpectSingleGlError(GL_INVALID_VALUE);
    }

    // The last legal index must stay silent - api_errors checks both sides of the boundary.
    MG_Impl::GLImpl::ViewportIndexedf(kMaxViewports - 1, 0.0f, 0.0f, 1.0f, 1.0f);
    ExpectSingleGlError(GL_NO_ERROR);
    MG_Impl::GLImpl::ScissorIndexed(kMaxViewports - 1, 0, 0, 1, 1);
    ExpectSingleGlError(GL_NO_ERROR);
    MG_Impl::GLImpl::DepthRangeIndexed(kMaxViewports - 1, 0.0, 1.0);
    ExpectSingleGlError(GL_NO_ERROR);
}

TEST_F(RenderStateTest, ArraySettersRejectAnOutOfRangeRangeButAcceptAnExactlyFullOne) {
    Array<GLfloat, kMaxViewports * 4> viewports{};
    Array<GLint, kMaxViewports * 4> scissors{};
    Array<GLdouble, kMaxViewports * 2> depths{};
    for (GLuint i = 0; i < kMaxViewports; ++i) {
        viewports[i * 4 + 2] = 1.0f;
        viewports[i * 4 + 3] = 1.0f;
        scissors[i * 4 + 2] = 1;
        scissors[i * 4 + 3] = 1;
        depths[i * 2 + 1] = 1.0;
    }

    // first == MAX_VIEWPORTS, and first + count > MAX_VIEWPORTS.
    MG_Impl::GLImpl::ViewportArrayv(kMaxViewports, 1, viewports.data());
    ExpectSingleGlError(GL_INVALID_VALUE);
    MG_Impl::GLImpl::ViewportArrayv(1, kMaxViewports, viewports.data());
    ExpectSingleGlError(GL_INVALID_VALUE);
    MG_Impl::GLImpl::ScissorArrayv(kMaxViewports, 1, scissors.data());
    ExpectSingleGlError(GL_INVALID_VALUE);
    MG_Impl::GLImpl::ScissorArrayv(1, kMaxViewports, scissors.data());
    ExpectSingleGlError(GL_INVALID_VALUE);
    MG_Impl::GLImpl::DepthRangeArrayv(kMaxViewports, 1, depths.data());
    ExpectSingleGlError(GL_INVALID_VALUE);
    MG_Impl::GLImpl::DepthRangeArrayv(1, kMaxViewports, depths.data());
    ExpectSingleGlError(GL_INVALID_VALUE);

    // first + count == MAX_VIEWPORTS is LEGAL - the off-by-one an ">=" bound would get wrong,
    // and one KHR-GL43.viewport_array.api_errors asserts explicitly.
    MG_Impl::GLImpl::ViewportArrayv(1, kMaxViewports - 1, viewports.data());
    ExpectSingleGlError(GL_NO_ERROR);
    MG_Impl::GLImpl::ScissorArrayv(1, kMaxViewports - 1, scissors.data());
    ExpectSingleGlError(GL_NO_ERROR);
    MG_Impl::GLImpl::DepthRangeArrayv(1, kMaxViewports - 1, depths.data());
    ExpectSingleGlError(GL_NO_ERROR);

    // A negative count is GL_INVALID_VALUE and must not be read as a huge unsigned length.
    MG_Impl::GLImpl::ViewportArrayv(0, -1, viewports.data());
    ExpectSingleGlError(GL_INVALID_VALUE);
    MG_Impl::GLImpl::ScissorArrayv(0, -1, scissors.data());
    ExpectSingleGlError(GL_INVALID_VALUE);
    MG_Impl::GLImpl::DepthRangeArrayv(0, -1, depths.data());
    ExpectSingleGlError(GL_INVALID_VALUE);
}

TEST_F(RenderStateTest, NegativeExtentsAreRejectedWithoutDisturbingState) {
    MG_Impl::GLImpl::Viewport(0, 0, 4, 4);
    MG_Impl::GLImpl::Scissor(0, 0, 4, 4);
    ExpectSingleGlError(GL_NO_ERROR);

    MG_Impl::GLImpl::Viewport(0, 0, -1, 1);
    ExpectSingleGlError(GL_INVALID_VALUE);
    MG_Impl::GLImpl::Viewport(0, 0, 1, -1);
    ExpectSingleGlError(GL_INVALID_VALUE);
    MG_Impl::GLImpl::Scissor(0, 0, -1, 1);
    ExpectSingleGlError(GL_INVALID_VALUE);
    MG_Impl::GLImpl::Scissor(0, 0, 1, -1);
    ExpectSingleGlError(GL_INVALID_VALUE);

    for (GLuint index = 0; index < kMaxViewports; ++index) {
        MG_Impl::GLImpl::ViewportIndexedf(index, 0.0f, 0.0f, -1.0f, 1.0f);
        ExpectSingleGlError(GL_INVALID_VALUE);
        MG_Impl::GLImpl::ViewportIndexedf(index, 0.0f, 0.0f, 1.0f, -1.0f);
        ExpectSingleGlError(GL_INVALID_VALUE);

        const GLfloat badW[4] = {0.0f, 0.0f, -1.0f, 1.0f};
        MG_Impl::GLImpl::ViewportIndexedfv(index, badW);
        ExpectSingleGlError(GL_INVALID_VALUE);

        MG_Impl::GLImpl::ScissorIndexed(index, 0, 0, -1, 1);
        ExpectSingleGlError(GL_INVALID_VALUE);
        const GLint badH[4] = {0, 0, 1, -1};
        MG_Impl::GLImpl::ScissorIndexedv(index, badH);
        ExpectSingleGlError(GL_INVALID_VALUE);

        // The array form must reject the WHOLE call for one bad element, exactly once, and
        // leave every rectangle alone - api_errors submits a full 16-element array with a
        // single negative extent and then requires the error queue to hold one entry.
        Array<GLfloat, kMaxViewports * 4> viewports{};
        Array<GLint, kMaxViewports * 4> scissors{};
        for (GLuint i = 0; i < kMaxViewports; ++i) {
            viewports[i * 4 + 2] = 1.0f;
            viewports[i * 4 + 3] = 1.0f;
            scissors[i * 4 + 2] = 1;
            scissors[i * 4 + 3] = 1;
        }
        viewports[index * 4 + 2] = -1.0f;
        scissors[index * 4 + 3] = -1;
        MG_Impl::GLImpl::ViewportArrayv(0, kMaxViewports, viewports.data());
        ExpectSingleGlError(GL_INVALID_VALUE);
        MG_Impl::GLImpl::ScissorArrayv(0, kMaxViewports, scissors.data());
        ExpectSingleGlError(GL_INVALID_VALUE);
    }

    // Nothing above may have landed.
    GLint viewport[4] = {};
    MG_Impl::GLImpl::GetIntegeri_v(GL_VIEWPORT, 0, viewport);
    EXPECT_EQ(viewport[2], 4);
    EXPECT_EQ(viewport[3], 4);
    GLint scissor[4] = {};
    MG_Impl::GLImpl::GetIntegeri_v(GL_SCISSOR_BOX, 0, scissor);
    EXPECT_EQ(scissor[2], 4);
    EXPECT_EQ(scissor[3], 4);
    ExpectSingleGlError(GL_NO_ERROR);
}

TEST_F(RenderStateTest, IndexedRectangleQueriesRejectAnOutOfRangeIndex) {
    GLint ints[4] = {};
    GLfloat floats[4] = {};
    GLdouble doubles[4] = {};

    MG_Impl::GLImpl::GetIntegeri_v(GL_SCISSOR_BOX, kMaxViewports, ints);
    ExpectSingleGlError(GL_INVALID_VALUE);
    MG_Impl::GLImpl::GetFloati_v(GL_VIEWPORT, kMaxViewports, floats);
    ExpectSingleGlError(GL_INVALID_VALUE);
    MG_Impl::GLImpl::GetDoublei_v(GL_DEPTH_RANGE, kMaxViewports, doubles);
    ExpectSingleGlError(GL_INVALID_VALUE);

    MG_Impl::GLImpl::GetIntegeri_v(GL_SCISSOR_BOX, kMaxViewports - 1, ints);
    ExpectSingleGlError(GL_NO_ERROR);
    MG_Impl::GLImpl::GetFloati_v(GL_VIEWPORT, kMaxViewports - 1, floats);
    ExpectSingleGlError(GL_NO_ERROR);
    MG_Impl::GLImpl::GetDoublei_v(GL_DEPTH_RANGE, kMaxViewports - 1, doubles);
    ExpectSingleGlError(GL_NO_ERROR);
}

// ---------------------------------------------------------------------------------------------
// "Has the application written this scissor rectangle?" - the flag, not the extent
// ---------------------------------------------------------------------------------------------
// glScissor(0, 0, 0, 0) is legal GL and means "the scissor test rejects every fragment", but it
// is byte-identical to the never-written default, whose meaning is the opposite ("the whole
// window", which the frontend cannot spell before a surface exists). DirectGLES resolved the two
// by looking at the EXTENT and so inverted every deliberately empty box into the full surface -
// KHR-GL43.viewport_array.scissor_zero_dimension is exactly that draw, and it came back holding
// the drawn colour where the untouched fill was required.
//
// These drive RenderState directly instead of the GL entry points on purpose: the flag's whole
// content is what it says BEFORE the first scissor call of a context, and this binary shares one
// context across every case in the file, so a pristine object is the only place that state
// still exists by the time these run.

namespace {
    constexpr Uint32 kAllViewportsWritten =
        RenderStateParameters::MAX_VIEWPORTS >= 32 ? ~0u : (1u << RenderStateParameters::MAX_VIEWPORTS) - 1u;
} // namespace

TEST_F(RenderStateTest, AnEmptyScissorBoxIsDistinguishableFromNeverHavingBeenWritten) {
    MG_State::GLState::RenderState state;
    EXPECT_EQ(state.GetAllParameters().ScissorBoxWrittenMask, 0u)
        << "a fresh context has never been given a scissor box, and the all-zero rectangle it "
           "starts with must not be mistaken for one";

    // Not one stored byte moves here - every box already held (0,0,0,0) - and yet this is the
    // call that turns "the frontend does not know the window size" into "reject every fragment".
    state.SetScissorBox(IntVec4(0, 0, 0, 0));
    EXPECT_EQ(state.GetAllParameters().ScissorBoxWrittenMask, kAllViewportsWritten);
    for (GLuint index = 0; index < kMaxViewports; ++index) {
        EXPECT_EQ(state.GetScissorBoxIndexed(index), IntVec4(0, 0, 0, 0)) << "index " << index;
    }
}

TEST_F(RenderStateTest, AnIndexedScissorWriteClaimsOnlyItsOwnIndex) {
    MG_State::GLState::RenderState state;
    state.SetScissorBoxIndexed(5, IntVec4(0, 0, 0, 0));
    EXPECT_EQ(state.GetAllParameters().ScissorBoxWrittenMask, 1u << 5);

    state.SetScissorBoxIndexed(0, IntVec4(0, 0, 0, 0));
    EXPECT_EQ(state.GetAllParameters().ScissorBoxWrittenMask, (1u << 5) | 1u);

    // ARB_viewport_array makes the non-indexed setter a write to every index, so it claims all 16.
    state.SetScissorBox(IntVec4(1, 2, 3, 4));
    EXPECT_EQ(state.GetAllParameters().ScissorBoxWrittenMask, kAllViewportsWritten);
}

TEST_F(RenderStateTest, TheFirstScissorWriteBumpsTheVersionEvenWhenTheValueDoesNotMove) {
    // Load-bearing, and not merely tidy: DirectGLES' SyncRenderState early-outs on an unchanged
    // render-state version BEFORE it reaches the span memcmp that would otherwise notice the
    // flag. A version-less transition would sit in the parameter block, never be pushed, and the
    // empty box would go on rendering as the whole surface.
    MG_State::GLState::RenderState state;
    const Uint initial = state.GetVersion();
    state.SetScissorBox(IntVec4(0, 0, 0, 0));
    EXPECT_GT(state.GetVersion(), initial) << "claiming the rectangle is itself a state change";

    // Once claimed, a genuinely redundant write stays free - the flag costs one transition, not
    // a version bump per call.
    const Uint settled = state.GetVersion();
    state.SetScissorBox(IntVec4(0, 0, 0, 0));
    EXPECT_EQ(state.GetVersion(), settled);

    MG_State::GLState::RenderState indexed;
    const Uint indexedInitial = indexed.GetVersion();
    indexed.SetScissorBoxIndexed(3, IntVec4(0, 0, 0, 0));
    EXPECT_GT(indexed.GetVersion(), indexedInitial);
    const Uint indexedSettled = indexed.GetVersion();
    indexed.SetScissorBoxIndexed(3, IntVec4(0, 0, 0, 0));
    EXPECT_EQ(indexed.GetVersion(), indexedSettled);
}

// --- glPatchParameterfv (GL 4.6 core 11.2.2) ---------------------------------------------------
//
// GL_PATCH_DEFAULT_OUTER_LEVEL / GL_PATCH_DEFAULT_INNER_LEVEL are the tessellation levels a
// program with an evaluation stage and NO control stage runs at. glPatchParameterfv was a stub
// that stored nothing and raised nothing, so the state could never move off its 1.0 default and
// both backends hardcoded 1.0 into the pass-through control stage they synthesize. The getters
// were absent too, which is what KHR-GL4x.tessellation_shader.single.
// default_values_of_context_wide_properties dies on.

TEST_F(RenderStateTest, PatchDefaultLevelsStartAtTheGLDefault) {
    GLfloat outer[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
    MG_Impl::GLImpl::GetFloatv(GL_PATCH_DEFAULT_OUTER_LEVEL, outer);
    ExpectSingleGlError(GL_NO_ERROR);
    for (const GLfloat level : outer) EXPECT_FLOAT_EQ(level, 1.0f);

    GLfloat inner[2] = {-1.0f, -1.0f};
    MG_Impl::GLImpl::GetFloatv(GL_PATCH_DEFAULT_INNER_LEVEL, inner);
    ExpectSingleGlError(GL_NO_ERROR);
    for (const GLfloat level : inner) EXPECT_FLOAT_EQ(level, 1.0f);
}

TEST_F(RenderStateTest, PatchDefaultLevelsRoundTripThroughEveryGetter) {
    const GLfloat outerIn[4] = {2.0f, 3.5f, 4.0f, 5.25f};
    MG_Impl::GLImpl::PatchParameterfv(GL_PATCH_DEFAULT_OUTER_LEVEL, outerIn);
    ExpectSingleGlError(GL_NO_ERROR);
    const GLfloat innerIn[2] = {6.5f, 7.0f};
    MG_Impl::GLImpl::PatchParameterfv(GL_PATCH_DEFAULT_INNER_LEVEL, innerIn);
    ExpectSingleGlError(GL_NO_ERROR);

    GLfloat outer[4] = {};
    MG_Impl::GLImpl::GetFloatv(GL_PATCH_DEFAULT_OUTER_LEVEL, outer);
    EXPECT_FLOAT_EQ(outer[0], 2.0f);
    EXPECT_FLOAT_EQ(outer[1], 3.5f);
    EXPECT_FLOAT_EQ(outer[2], 4.0f);
    EXPECT_FLOAT_EQ(outer[3], 5.25f);
    GLfloat inner[2] = {};
    MG_Impl::GLImpl::GetFloatv(GL_PATCH_DEFAULT_INNER_LEVEL, inner);
    EXPECT_FLOAT_EQ(inner[0], 6.5f);
    EXPECT_FLOAT_EQ(inner[1], 7.0f);
    ExpectSingleGlError(GL_NO_ERROR);

    // Float state read through the integer and boolean getters: glGetIntegerv rounds (GL 4.6 core
    // 2.2.2) and glGetBooleanv delegates to it, so both must ANSWER rather than report
    // INVALID_ENUM - which is exactly what the conformance suite asks them first.
    GLint outerInts[4] = {};
    MG_Impl::GLImpl::GetIntegerv(GL_PATCH_DEFAULT_OUTER_LEVEL, outerInts);
    EXPECT_EQ(outerInts[0], 2);
    EXPECT_EQ(outerInts[1], 4) << "3.5 rounds away from zero";
    EXPECT_EQ(outerInts[3], 5);
    ExpectSingleGlError(GL_NO_ERROR);

    GLboolean outerBools[4] = {};
    MG_Impl::GLImpl::GetBooleanv(GL_PATCH_DEFAULT_OUTER_LEVEL, outerBools);
    EXPECT_EQ(outerBools[0], GL_TRUE);
    ExpectSingleGlError(GL_NO_ERROR);

    GLdouble outerDoubles[4] = {};
    MG_Impl::GLImpl::GetDoublev(GL_PATCH_DEFAULT_OUTER_LEVEL, outerDoubles);
    EXPECT_DOUBLE_EQ(outerDoubles[3], 5.25) << "glGetDoublev must widen all four, not just the first";
    ExpectSingleGlError(GL_NO_ERROR);

    // glGetInteger64v shares glGetIntegerv's accepted-pname set (GL 4.6 core 22.1), so it owes the
    // same component count. Its own table listed neither pname, so three of the four words were
    // left holding whatever the caller's buffer held - and no error said so.
    GLint64 outerLongs[4] = {9, 9, 9, 9};
    MG_Impl::GLImpl::GetInteger64v(GL_PATCH_DEFAULT_OUTER_LEVEL, outerLongs);
    EXPECT_EQ(outerLongs[0], 2);
    EXPECT_EQ(outerLongs[3], 5) << "glGetInteger64v must write all four, not just the first";
    GLint64 innerLongs[2] = {9, 9};
    MG_Impl::GLImpl::GetInteger64v(GL_PATCH_DEFAULT_INNER_LEVEL, innerLongs);
    EXPECT_EQ(innerLongs[1], 7);
    ExpectSingleGlError(GL_NO_ERROR);

    // Put the context back where the rest of the binary expects it.
    const GLfloat defaults4[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    const GLfloat defaults2[2] = {1.0f, 1.0f};
    MG_Impl::GLImpl::PatchParameterfv(GL_PATCH_DEFAULT_OUTER_LEVEL, defaults4);
    MG_Impl::GLImpl::PatchParameterfv(GL_PATCH_DEFAULT_INNER_LEVEL, defaults2);
    DrainPendingGlErrors();
}

// GL 4.6 core 2.2.2: a float state comes back through glGetBooleanv as GL_FALSE only when it is
// zero. Deriving the answer from glGetIntegerv - which rounds - reported GL_FALSE for a level of
// 0.25, which is neither zero nor anything the application asked to be rounded.
TEST_F(RenderStateTest, PatchDefaultLevelsBelowHalfAreStillTrueAsBooleans) {
    const GLfloat fractional[4] = {0.25f, 0.0f, 0.4f, 0.25f};
    MG_Impl::GLImpl::PatchParameterfv(GL_PATCH_DEFAULT_OUTER_LEVEL, fractional);
    const GLfloat fractionalInner[2] = {0.25f, 0.0f};
    MG_Impl::GLImpl::PatchParameterfv(GL_PATCH_DEFAULT_INNER_LEVEL, fractionalInner);
    ExpectSingleGlError(GL_NO_ERROR);

    GLboolean outer[4] = {};
    MG_Impl::GLImpl::GetBooleanv(GL_PATCH_DEFAULT_OUTER_LEVEL, outer);
    EXPECT_EQ(outer[0], GL_TRUE) << "0.25 is not zero";
    EXPECT_EQ(outer[1], GL_FALSE) << "0.0 is the one value that is false";
    EXPECT_EQ(outer[2], GL_TRUE);
    GLboolean inner[2] = {};
    MG_Impl::GLImpl::GetBooleanv(GL_PATCH_DEFAULT_INNER_LEVEL, inner);
    EXPECT_EQ(inner[0], GL_TRUE);
    EXPECT_EQ(inner[1], GL_FALSE);
    ExpectSingleGlError(GL_NO_ERROR);

    const GLfloat defaults4[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    const GLfloat defaults2[2] = {1.0f, 1.0f};
    MG_Impl::GLImpl::PatchParameterfv(GL_PATCH_DEFAULT_OUTER_LEVEL, defaults4);
    MG_Impl::GLImpl::PatchParameterfv(GL_PATCH_DEFAULT_INNER_LEVEL, defaults2);
    DrainPendingGlErrors();
}

TEST_F(RenderStateTest, PatchParameterfvRejectsEveryOtherPname) {
    const GLfloat levels[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    MG_Impl::GLImpl::PatchParameterfv(GL_PATCH_VERTICES, levels);
    ExpectSingleGlError(GL_INVALID_ENUM);
    MG_Impl::GLImpl::PatchParameterfv(GL_MAX_PATCH_VERTICES, levels);
    ExpectSingleGlError(GL_INVALID_ENUM);
    // The integer setter keeps its own, disjoint, accepted pname.
    MG_Impl::GLImpl::PatchParameteri(GL_PATCH_DEFAULT_OUTER_LEVEL, 4);
    ExpectSingleGlError(GL_INVALID_ENUM);
}

TEST_F(RenderStateTest, PatchDefaultLevelsAreTreatedAsPipelineState) {
    // Load-bearing: both backends compile these numbers into the pass-through tessellation control
    // stage they synthesize, so a change has to invalidate an already-built program the same way a
    // glPatchParameteri does. Bumping only the all-state version would leave DirectVulkan's
    // pipeline memo - which keys on the PIPELINE-state version - handing back a pipeline built
    // with the old levels.
    MG_State::GLState::RenderState state;
    const Uint initialPipelineVersion = state.GetPipelineStateVersion();
    state.SetPatchDefaultOuterLevel(FloatVec4(2.0f, 2.0f, 2.0f, 2.0f));
    EXPECT_GT(state.GetPipelineStateVersion(), initialPipelineVersion);

    const Uint settled = state.GetPipelineStateVersion();
    state.SetPatchDefaultOuterLevel(FloatVec4(2.0f, 2.0f, 2.0f, 2.0f));
    EXPECT_EQ(state.GetPipelineStateVersion(), settled) << "a redundant write is free";

    state.SetPatchDefaultInnerLevel(FloatVec2(3.0f, 3.0f));
    EXPECT_GT(state.GetPipelineStateVersion(), settled);
}

// glPatchParameterfv accepts NaN by design, and NaN is never equal to itself under IEEE `==`. A
// value-compared redundant-write guard therefore never settles: every re-set of the identical
// tuple bumps the pipeline-state version, and - one level down - DirectGLES's staleness clause
// re-transpiles, re-compiles and re-links the synthesized pass-through stage on every draw. Both
// compare BIT PATTERNS instead, which is what DirectVulkan's module key already hashes.
TEST_F(RenderStateTest, ARedundantNaNPatchLevelWriteSettlesInsteadOfBumpingForever) {
    const Float notANumber = std::numeric_limits<Float>::quiet_NaN();
    MG_State::GLState::RenderState state;
    state.SetPatchDefaultOuterLevel(FloatVec4(notANumber, 1.0f, 1.0f, 1.0f));
    const Uint afterFirst = state.GetPipelineStateVersion();

    state.SetPatchDefaultOuterLevel(FloatVec4(notANumber, 1.0f, 1.0f, 1.0f));
    EXPECT_EQ(state.GetPipelineStateVersion(), afterFirst)
        << "the identical NaN tuple is not a state change";

    state.SetPatchDefaultInnerLevel(FloatVec2(notANumber, 1.0f));
    const Uint afterInner = state.GetPipelineStateVersion();
    state.SetPatchDefaultInnerLevel(FloatVec2(notANumber, 1.0f));
    EXPECT_EQ(state.GetPipelineStateVersion(), afterInner);

    // A genuinely different tuple still moves, so the guard has not simply gone blind.
    state.SetPatchDefaultOuterLevel(FloatVec4(notANumber, 2.0f, 1.0f, 1.0f));
    EXPECT_GT(state.GetPipelineStateVersion(), afterInner);
}

// --- desktop GL_PRIMITIVE_RESTART state --------------------------------------------------------
//
// The cap and its index are what a desktop application enables instead of ES's
// GL_PRIMITIVE_RESTART_FIXED_INDEX. Both halves have to be answerable, because the backends read
// them on every indexed draw to decide whether the index data needs rewriting.
TEST_F(RenderStateTest, PrimitiveRestartCapAndIndexAreBothQueryable) {
    EXPECT_EQ(MG_Impl::GLImpl::IsEnabled(GL_PRIMITIVE_RESTART), GL_FALSE);
    ExpectSingleGlError(GL_NO_ERROR);

    MG_Impl::GLImpl::Enable(GL_PRIMITIVE_RESTART);
    MG_Impl::GLImpl::PrimitiveRestartIndex(1026u);
    ExpectSingleGlError(GL_NO_ERROR);

    EXPECT_EQ(MG_Impl::GLImpl::IsEnabled(GL_PRIMITIVE_RESTART), GL_TRUE);
    GLint index = 0;
    MG_Impl::GLImpl::GetIntegerv(GL_PRIMITIVE_RESTART_INDEX, &index);
    EXPECT_EQ(index, 1026);
    // The fixed-index cap is a separate piece of state and must not have moved.
    EXPECT_EQ(MG_Impl::GLImpl::IsEnabled(GL_PRIMITIVE_RESTART_FIXED_INDEX), GL_FALSE);
    ExpectSingleGlError(GL_NO_ERROR);

    MG_Impl::GLImpl::Disable(GL_PRIMITIVE_RESTART);
    MG_Impl::GLImpl::PrimitiveRestartIndex(0u);
    DrainPendingGlErrors();
}
