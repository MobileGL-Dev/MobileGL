// MobileGL - MobileGL/MG_Test/State/DebugTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// KHR_debug (GL 4.6 core 20), the part MobileGL actually implements: the debug group stack and
// object labels. These were silent stubs - glPushDebugGroup logged once and returned, glObjectLabel
// discarded its argument and glGetObjectLabel always answered with an empty string - which meant
// GL_DEBUG_GROUP_STACK_DEPTH reported 0 (not a legal value; the context is created with one group
// already on the stack) and a label never survived being written.
//
// The calls are deliberately NOT forwarded to the host driver; GL_Debug.h explains why. What the
// tests below pin is the observable contract that remains: the stack depth is real and its
// over/underflow errors are the ones KHR_debug names, and a label written comes back.

#include <gtest/gtest.h>

#include <string>

#include "Includes.h"
#include "Init.h"
#include <MG_Impl/GLImpl/Buffer/GL_Buffer.h>
#include <MG_Impl/GLImpl/Debug/GL_Debug.h>
#include <MG_Impl/GLImpl/Getter/GL_Getter.h>
#include <MG_Impl/GLImpl/Texture/GL_Texture.h>
#include <MG_State/GLState/Core.h>

using namespace MobileGL;

namespace {
    class DebugTest : public ::testing::Test {
    protected:
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
            // The group stack is context state and this binary shares one context across cases,
            // so unwind whatever a previous case left pushed.
            while (StackDepth() > 1) {
                MG_Impl::GLImpl::PopDebugGroup();
            }
            DrainPendingGlErrors();
        }

        void TearDown() override {
            while (StackDepth() > 1) {
                MG_Impl::GLImpl::PopDebugGroup();
            }
            DrainPendingGlErrors();
        }

        static GLint StackDepth() {
            GLint depth = -1;
            MG_Impl::GLImpl::GetIntegerv(GL_DEBUG_GROUP_STACK_DEPTH, &depth);
            return depth;
        }

        static GLuint GenTexture() {
            GLuint texture = 0;
            MG_Impl::GLImpl::GenTextures(1, &texture);
            return texture;
        }
    };

    TEST_F(DebugTest, StackDepthStartsAtOneAndTracksPushesAndPops) {
        // GL 4.6 core 20.6: the context is created with one group on the stack, so 0 is never a
        // legal answer - which is what the old stub reported.
        EXPECT_EQ(StackDepth(), 1);

        MG_Impl::GLImpl::PushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 1, -1, "outer");
        ExpectSingleGlError(GL_NO_ERROR);
        EXPECT_EQ(StackDepth(), 2);

        MG_Impl::GLImpl::PushDebugGroup(GL_DEBUG_SOURCE_THIRD_PARTY, 2, -1, "inner");
        ExpectSingleGlError(GL_NO_ERROR);
        EXPECT_EQ(StackDepth(), 3);

        MG_Impl::GLImpl::PopDebugGroup();
        ExpectSingleGlError(GL_NO_ERROR);
        EXPECT_EQ(StackDepth(), 2);

        MG_Impl::GLImpl::PopDebugGroup();
        ExpectSingleGlError(GL_NO_ERROR);
        EXPECT_EQ(StackDepth(), 1);
    }

    TEST_F(DebugTest, PoppingTheBaseGroupIsStackUnderflow) {
        ASSERT_EQ(StackDepth(), 1);
        MG_Impl::GLImpl::PopDebugGroup();
        ExpectSingleGlError(GL_STACK_UNDERFLOW);
        EXPECT_EQ(StackDepth(), 1) << "a refused pop must not move the stack";
    }

    TEST_F(DebugTest, PushingPastTheAdvertisedLimitIsStackOverflow) {
        GLint limit = 0;
        MG_Impl::GLImpl::GetIntegerv(GL_MAX_DEBUG_GROUP_STACK_DEPTH, &limit);
        ASSERT_GE(limit, 64) << "KHR_debug floors GL_MAX_DEBUG_GROUP_STACK_DEPTH at 64";

        // Nesting exactly to the advertised limit must WORK - an implementation whose real limit
        // is lower than the one it reports is worse than one that reports a lower limit.
        for (GLint i = 1; i < limit; ++i) {
            MG_Impl::GLImpl::PushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, -1, "deep");
        }
        DrainPendingGlErrors();
        EXPECT_EQ(StackDepth(), limit);

        MG_Impl::GLImpl::PushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, -1, "too deep");
        ExpectSingleGlError(GL_STACK_OVERFLOW);
        EXPECT_EQ(StackDepth(), limit) << "a refused push must not move the stack";
    }

    TEST_F(DebugTest, OnlyApplicationAndThirdPartySourcesMayBePushed) {
        // 20.2 reserves every other source for the implementation.
        MG_Impl::GLImpl::PushDebugGroup(GL_DEBUG_SOURCE_API, 0, -1, "not mine to push");
        ExpectSingleGlError(GL_INVALID_ENUM);
        EXPECT_EQ(StackDepth(), 1);
    }

    TEST_F(DebugTest, DebugMessageInsertValidatesItsEnums) {
        MG_Impl::GLImpl::DebugMessageInsert(GL_DEBUG_SOURCE_APPLICATION, GL_DEBUG_TYPE_MARKER, 0,
                                            GL_DEBUG_SEVERITY_NOTIFICATION, -1, "hello");
        ExpectSingleGlError(GL_NO_ERROR);

        MG_Impl::GLImpl::DebugMessageInsert(GL_DEBUG_SOURCE_API, GL_DEBUG_TYPE_MARKER, 0,
                                            GL_DEBUG_SEVERITY_NOTIFICATION, -1, "bad source");
        ExpectSingleGlError(GL_INVALID_ENUM);

        MG_Impl::GLImpl::DebugMessageInsert(GL_DEBUG_SOURCE_APPLICATION, GL_TEXTURE_2D, 0,
                                            GL_DEBUG_SEVERITY_NOTIFICATION, -1, "bad type");
        ExpectSingleGlError(GL_INVALID_ENUM);

        MG_Impl::GLImpl::DebugMessageInsert(GL_DEBUG_SOURCE_APPLICATION, GL_DEBUG_TYPE_MARKER, 0, GL_TEXTURE_2D, -1,
                                            "bad severity");
        ExpectSingleGlError(GL_INVALID_ENUM);
    }

    TEST_F(DebugTest, AMessageLongerThanTheAdvertisedLimitIsInvalidValue) {
        GLint limit = 0;
        MG_Impl::GLImpl::GetIntegerv(GL_MAX_DEBUG_MESSAGE_LENGTH, &limit);
        ASSERT_GT(limit, 0);
        const std::string tooLong(static_cast<std::size_t>(limit) + 1, 'x');

        MG_Impl::GLImpl::DebugMessageInsert(GL_DEBUG_SOURCE_APPLICATION, GL_DEBUG_TYPE_MARKER, 0,
                                            GL_DEBUG_SEVERITY_NOTIFICATION, -1, tooLong.c_str());
        ExpectSingleGlError(GL_INVALID_VALUE);
    }

    TEST_F(DebugTest, ALabelWrittenComesBack) {
        const GLuint texture = GenTexture();
        MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
        DrainPendingGlErrors();

        MG_Impl::GLImpl::ObjectLabel(GL_TEXTURE, texture, -1, "coverage_stencil");
        ExpectSingleGlError(GL_NO_ERROR);

        GLchar buffer[64] = {};
        GLsizei length = -1;
        MG_Impl::GLImpl::GetObjectLabel(GL_TEXTURE, texture, sizeof(buffer), &length, buffer);
        ExpectSingleGlError(GL_NO_ERROR);
        // 20.5: the returned length excludes the terminator.
        EXPECT_EQ(length, static_cast<GLsizei>(std::string("coverage_stencil").size()));
        EXPECT_STREQ(buffer, "coverage_stencil");
    }

    TEST_F(DebugTest, LabelsAreScopedToTheObjectAndItsType) {
        const GLuint texture = GenTexture();
        MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
        GLuint buffer = 0;
        MG_Impl::GLImpl::GenBuffers(1, &buffer);
        MG_Impl::GLImpl::BindBuffer(GL_ARRAY_BUFFER, buffer);
        DrainPendingGlErrors();

        MG_Impl::GLImpl::ObjectLabel(GL_TEXTURE, texture, -1, "the texture");
        MG_Impl::GLImpl::ObjectLabel(GL_BUFFER, buffer, -1, "the buffer");
        DrainPendingGlErrors();

        GLchar textureLabel[32] = {};
        GLchar bufferLabel[32] = {};
        MG_Impl::GLImpl::GetObjectLabel(GL_TEXTURE, texture, sizeof(textureLabel), nullptr, textureLabel);
        MG_Impl::GLImpl::GetObjectLabel(GL_BUFFER, buffer, sizeof(bufferLabel), nullptr, bufferLabel);
        DrainPendingGlErrors();
        // The two names may collide numerically - they are separate namespaces - so a label store
        // keyed on the name alone would hand one object's label to the other.
        EXPECT_STREQ(textureLabel, "the texture");
        EXPECT_STREQ(bufferLabel, "the buffer");
    }

    TEST_F(DebugTest, AnUnlabelledObjectAnswersWithAnEmptyString) {
        const GLuint texture = GenTexture();
        MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
        DrainPendingGlErrors();

        GLchar buffer[8] = {'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x'};
        GLsizei length = -1;
        MG_Impl::GLImpl::GetObjectLabel(GL_TEXTURE, texture, sizeof(buffer), &length, buffer);
        ExpectSingleGlError(GL_NO_ERROR);
        EXPECT_EQ(length, 0);
        EXPECT_STREQ(buffer, "");
    }

    TEST_F(DebugTest, ALabelIsTruncatedToTheBufferAndStaysTerminated) {
        const GLuint texture = GenTexture();
        MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
        MG_Impl::GLImpl::ObjectLabel(GL_TEXTURE, texture, -1, "abcdefgh");
        DrainPendingGlErrors();

        GLchar buffer[4] = {};
        GLsizei length = -1;
        MG_Impl::GLImpl::GetObjectLabel(GL_TEXTURE, texture, sizeof(buffer), &length, buffer);
        ExpectSingleGlError(GL_NO_ERROR);
        EXPECT_EQ(length, 3) << "bufSize includes the terminator, so only bufSize-1 characters fit";
        EXPECT_STREQ(buffer, "abc");
    }

    TEST_F(DebugTest, LabellingSomethingThatDoesNotExistIsInvalidValue) {
        MG_Impl::GLImpl::ObjectLabel(GL_TEXTURE, 0xFFFFFFFFu, -1, "nothing");
        ExpectSingleGlError(GL_INVALID_VALUE);
    }

    TEST_F(DebugTest, LabellingANonObjectTypeIsInvalidEnum) {
        const GLuint texture = GenTexture();
        MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
        DrainPendingGlErrors();
        MG_Impl::GLImpl::ObjectLabel(GL_TEXTURE_2D, texture, -1, "not an object type");
        ExpectSingleGlError(GL_INVALID_ENUM);
    }

    TEST_F(DebugTest, ANullLabelRemovesTheLabel) {
        const GLuint texture = GenTexture();
        MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, texture);
        MG_Impl::GLImpl::ObjectLabel(GL_TEXTURE, texture, -1, "temporary");
        MG_Impl::GLImpl::ObjectLabel(GL_TEXTURE, texture, 0, nullptr);
        DrainPendingGlErrors();

        GLsizei length = -1;
        GLchar buffer[16] = {};
        MG_Impl::GLImpl::GetObjectLabel(GL_TEXTURE, texture, sizeof(buffer), &length, buffer);
        ExpectSingleGlError(GL_NO_ERROR);
        EXPECT_EQ(length, 0);
    }
} // namespace
