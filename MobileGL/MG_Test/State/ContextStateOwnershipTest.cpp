// MobileGL - MobileGL/MG_Test/State/ContextStateOwnershipTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P14 S3 (docs/Disaggregated/design/11-state-ownership.md section 4). The frontend's GL state
// belongs to an EGL context, its objects and name spaces to a share group:
//   * two contexts that do not share have their own name spaces - the same numeric name is a
//     different object in each;
//   * two contexts that share see one object and keep their own bindings;
//   * an eglMakeCurrent switch moves errors, viewport and framebuffer bindings with it;
//   * eglDestroyContext drops one context's GL state and leaves the other's alone.
//
// GPU-free: every assertion is frontend state, reached through the real EGL and GL entry points.

#include <gtest/gtest.h>

#include "Includes.h"
#include "Init.h"
#include <MG_State/EGLState/Core.h>
#include <MG_State/GLState/Core.h>
#include <MG_Impl/GLImpl/Buffer/GL_Buffer.h>
#include <MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.h>
#include <MG_Impl/GLImpl/Getter/GL_Getter.h>
#include <MG_Impl/GLImpl/RenderState/GL_RenderState.h>

using namespace MobileGL;
using namespace MobileGL::MG_Impl::GLImpl;

namespace {
    using EGLStateContext = MG_State::EGLState::EGLContext;
    using GLStateContext = MG_State::GLState::GLContext;

    class ContextStateOwnershipTest : public ::testing::Test {
    protected:
        void SetUp() override {
            MobileGL::Initialize();
            State = MG_State::pEGLContext.get();
            ASSERT_NE(State, nullptr);

            Display = State->GetDisplay(EGL_DEFAULT_DISPLAY);
            ASSERT_NE(Display, EGL_NO_DISPLAY);
            ASSERT_TRUE(State->InitializeDisplay(Display, nullptr, nullptr));

            EGLint configCount = 0;
            ASSERT_TRUE(State->ChooseConfig(Display, nullptr, &Config, 1, &configCount));
            ASSERT_NE(Config, nullptr);

            const EGLint surfaceAttribs[] = {EGL_WIDTH, 4, EGL_HEIGHT, 4, EGL_NONE};
            Surface = State->CreatePbufferSurface(Display, Config, surfaceAttribs);
            ASSERT_NE(Surface, EGL_NO_SURFACE);
        }

        void TearDown() override {
            Release();
            for (const auto context : m_contexts) {
                if (State->ValidateContext(context)) State->DestroyContext(Display, context);
            }
            if (State->ValidateSurface(Surface)) State->DestroySurface(Display, Surface);
        }

        EGLContext NewContext(EGLContext shareWith) {
            const auto context = State->CreateContext(Display, Config, shareWith, nullptr);
            EXPECT_NE(context, EGL_NO_CONTEXT);
            m_contexts.push_back(context);
            return context;
        }

        void MakeCurrent(EGLContext context) {
            ASSERT_TRUE(State->MakeCurrent(Display, Surface, Surface, context));
        }

        void Release() {
            if (State) State->MakeCurrent(EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }

        static void DrainErrors() {
            for (Int drained = 0; drained < 16 && GetError() != GL_NO_ERROR; ++drained) {
            }
        }

        EGLStateContext* State = nullptr;
        EGLDisplay Display = EGL_NO_DISPLAY;
        EGLConfig Config = nullptr;
        EGLSurface Surface = EGL_NO_SURFACE;
        Vector<EGLContext> m_contexts;
    };
} // namespace

TEST_F(ContextStateOwnershipTest, NonsharedContextsKeepTheirOwnNameSpaces) {
    const auto contextA = NewContext(EGL_NO_CONTEXT);
    const auto contextB = NewContext(EGL_NO_CONTEXT);
    EXPECT_NE(State->GetContextShareGroupToken(contextA), State->GetContextShareGroupToken(contextB));
    EXPECT_NE(State->GetContextGLState(contextA), State->GetContextGLState(contextB));

    MakeCurrent(contextA);
    GLuint namesA[2] = {};
    GenBuffers(2, namesA);
    ASSERT_NE(namesA[0], 0u);
    BindBuffer(GL_ARRAY_BUFFER, namesA[0]);
    const auto objectA = MG_State::pGLContext->GetBufferObject(namesA[0]);
    ASSERT_NE(objectA, nullptr);
    EXPECT_EQ(MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Vertex).GetBoundObject(), objectA);

    MakeCurrent(contextB);
    // Neither name A handed out is a name here, and A's binding is not this thread's.
    EXPECT_FALSE(MG_State::pGLContext->ValidateBufferName(namesA[0]));
    EXPECT_FALSE(MG_State::pGLContext->ValidateBufferName(namesA[1]));
    EXPECT_EQ(MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Vertex).GetBoundObject(), nullptr);

    GLuint nameB = 0;
    GenBuffers(1, &nameB);
    // The generator is per share group, so the same numeric name comes out - as a DIFFERENT
    // object, because the table it names is a different one.
    EXPECT_EQ(nameB, namesA[0]);
    BindBuffer(GL_ARRAY_BUFFER, nameB);
    const auto objectB = MG_State::pGLContext->GetBufferObject(nameB);
    ASSERT_NE(objectB, nullptr);
    EXPECT_NE(objectB, objectA);

    MakeCurrent(contextA);
    EXPECT_EQ(MG_State::pGLContext->GetBufferObject(namesA[0]), objectA);
    EXPECT_EQ(MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Vertex).GetBoundObject(), objectA);
    EXPECT_TRUE(MG_State::pGLContext->ValidateBufferName(namesA[1]));
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

TEST_F(ContextStateOwnershipTest, SharingContextsSeeOneObjectButKeepTheirOwnBindings) {
    const auto contextA = NewContext(EGL_NO_CONTEXT);
    MakeCurrent(contextA);
    GLuint buffer = 0;
    GenBuffers(1, &buffer);
    BindBuffer(GL_ARRAY_BUFFER, buffer);
    const auto object = MG_State::pGLContext->GetBufferObject(buffer);
    ASSERT_NE(object, nullptr);

    const auto contextB = NewContext(contextA);
    EXPECT_EQ(State->GetContextShareGroupToken(contextB), State->GetContextShareGroupToken(contextA));
    EXPECT_NE(State->GetContextClientToken(contextB), State->GetContextClientToken(contextA));

    MakeCurrent(contextB);
    EXPECT_TRUE(MG_State::pGLContext->ValidateBufferName(buffer));
    EXPECT_EQ(MG_State::pGLContext->GetBufferObject(buffer), object);
    // The OBJECT is shared; the binding is not.
    EXPECT_EQ(MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Vertex).GetBoundObject(), nullptr);

    BindBuffer(GL_ARRAY_BUFFER, buffer);
    MakeCurrent(contextA);
    // A bound it before B did, and B's bind did not move A's slot.
    EXPECT_EQ(MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Vertex).GetBoundObject(), object);

    // One name space for the pair: a name B generates is a name A knows.
    MakeCurrent(contextB);
    GLuint fromB = 0;
    GenBuffers(1, &fromB);
    MakeCurrent(contextA);
    EXPECT_TRUE(MG_State::pGLContext->ValidateBufferName(fromB));
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

TEST_F(ContextStateOwnershipTest, MakeCurrentMovesErrorsViewportAndFramebufferBindings) {
    const auto contextA = NewContext(EGL_NO_CONTEXT);
    const auto contextB = NewContext(EGL_NO_CONTEXT);

    MakeCurrent(contextA);
    Viewport(1, 2, 3, 4);
    GLuint framebufferA = 0;
    GenFramebuffers(1, &framebufferA);
    ASSERT_NE(framebufferA, 0u);
    BindFramebuffer(GL_FRAMEBUFFER, framebufferA);
    const auto boundInA = MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject();
    ASSERT_NE(boundInA, nullptr);
    EXPECT_EQ(boundInA->GetExternalIndex(), framebufferA);
    DrainErrors();

    // A rejected call leaves A's viewport where it was and leaves A an error to report; the
    // error is NOT consumed here, so that the switch below can show whose list it is in.
    Viewport(0, 0, -1, -1);
    EXPECT_EQ(MG_State::pGLContext->GetViewport(), IntVec4(1, 2, 3, 4));

    MakeCurrent(contextB);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
    EXPECT_NE(MG_State::pGLContext->GetViewport(), IntVec4(1, 2, 3, 4));
    const auto boundInB = MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject();
    EXPECT_NE(boundInB == nullptr ? 0u : boundInB->GetExternalIndex(), framebufferA);

    MakeCurrent(contextA);
    EXPECT_EQ(MG_State::pGLContext->GetViewport(), IntVec4(1, 2, 3, 4));
    EXPECT_EQ(MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject(), boundInA);
    EXPECT_EQ(GetError(), GL_INVALID_VALUE);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

TEST_F(ContextStateOwnershipTest, DestroyingOneContextLeavesTheOtherAlone) {
    const auto contextA = NewContext(EGL_NO_CONTEXT);
    const auto contextB = NewContext(contextA);

    MakeCurrent(contextA);
    GLuint buffer = 0;
    GenBuffers(1, &buffer);
    BindBuffer(GL_ARRAY_BUFFER, buffer);
    const auto object = MG_State::pGLContext->GetBufferObject(buffer);
    ASSERT_NE(object, nullptr);

    MakeCurrent(contextB);
    BindBuffer(GL_ARRAY_BUFFER, buffer);

    // A is current on no thread, so EGL accepts the destroy.
    ASSERT_TRUE(State->DestroyContext(Display, contextA));
    EXPECT_FALSE(State->ValidateContext(contextA));
    EXPECT_EQ(State->GetContextGLState(contextA), nullptr);

    // B keeps its object - the share group outlives the context that opened it - and its binding.
    EXPECT_EQ(MG_State::pGLContext, State->GetContextGLState(contextB));
    EXPECT_EQ(MG_State::pGLContext->GetBufferObject(buffer), object);
    EXPECT_EQ(MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Vertex).GetBoundObject(), object);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

TEST_F(ContextStateOwnershipTest, ReleasingAnEglContextRestoresTheThreadsPreviousGLState) {
    const auto contextA = NewContext(EGL_NO_CONTEXT);
    const auto before = MG_State::pGLContext;
    ASSERT_NE(before, nullptr);

    MakeCurrent(contextA);
    EXPECT_EQ(MG_State::pGLContext, State->GetContextGLState(contextA));
    EXPECT_NE(MG_State::pGLContext, before);

    Release();
    EXPECT_EQ(MG_State::pGLContext, before);
}

TEST_F(ContextStateOwnershipTest, AThreadWithoutACurrentContextUsesTheProcessDefaultContext) {
    const auto processDefault = MG_State::pGLContext;
    ASSERT_NE(processDefault, nullptr);
    EXPECT_EQ(processDefault, MG_State::ProcessDefaultGLContext());

    const auto contextA = NewContext(EGL_NO_CONTEXT);
    EXPECT_NE(State->GetContextGLState(contextA), processDefault);

    MakeCurrent(contextA);
    EXPECT_EQ(MG_State::pGLContext, State->GetContextGLState(contextA));
    Release();
    EXPECT_EQ(MG_State::pGLContext, processDefault);
    EXPECT_NE(processDefault->GetShareGroup(), State->GetContextGLState(contextA)->GetShareGroup());
}
