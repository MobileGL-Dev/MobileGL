// MobileGL - MobileGL/MG_Test/VertexArray/VertexAttribBindingStateTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The ARB_vertex_attrib_binding state model, replayed exactly as
// KHR-GL4x.vertex_attrib_binding.basic-state1/3/4 and .negative-* walk it
// (external/openglcts/modules/gl/gl4cVertexAttribBindingTests.cpp): after each mutation the
// ten per-attribute pnames and the four per-binding-point pnames are read back in full, which
// is what makes a single wrong field visible as itself instead of as a downstream render
// difference.
//
// Four defects are pinned here, all of them frontend-only (both backends reported them
// byte-identically):
//   * VERTEX_BINDING_STRIDE defaulted to 0; the spec's initial value is 16.
//   * The eager binding -> attribute resolve overwrote VERTEX_ATTRIB_ARRAY_STRIDE / _POINTER,
//     which are legacy state only glVertexAttrib*Pointer may write.
//   * glVertexAttribDivisor did not re-point the attribute at its own binding point, so a
//     later resolve restored the old binding's divisor.
//   * The binding entry points accepted the default vertex array (name 0) in a core profile.
//
// GPU-free: this is all GL object state, no backend is consulted.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "Includes.h"
#include "Init.h"
#include <Config.h>
#include <MG_Impl/GLImpl/Buffer/GL_Buffer.h>
#include <MG_Impl/GLImpl/Getter/GL_Getter.h>
#include <MG_Impl/GLImpl/VertexArray/GL_VertexArray.h>
#include <MG_State/EGLState/Core.h>
#include <MG_State/GLState/Core.h>

using namespace MobileGL;
using namespace MobileGL::MG_Impl::GLImpl;

namespace {

    // Mirrors the CTS's VertexAttribState: the initial per-attribute state, mutated field by
    // field as the sequence proceeds, and verified in full after every call.
    struct AttribState {
        explicit AttribState(GLuint attribIndex) : index(attribIndex), binding(attribIndex) {}

        GLuint index = 0;
        GLint enabled = 0;
        GLint size = 4;
        GLint stride = 0;
        GLenum type = GL_FLOAT;
        GLint normalized = 0;
        GLint integer = 0;
        GLint isLong = 0;
        GLint divisor = 0;
        GLuint pointer = 0;
        GLuint bufferBinding = 0;
        GLuint binding = 0;
        GLint relativeOffset = 0;

        void Verify(const char* where) const {
            GLint p = -1;
            GetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &p);
            EXPECT_EQ(p, enabled) << where << ": ENABLED(" << index << ")";
            GetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_SIZE, &p);
            EXPECT_EQ(p, size) << where << ": SIZE(" << index << ")";
            GetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &p);
            EXPECT_EQ(p, stride) << where << ": STRIDE(" << index << ")";
            GetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_TYPE, &p);
            EXPECT_EQ(static_cast<GLenum>(p), type) << where << ": TYPE(" << index << ")";
            GetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &p);
            EXPECT_EQ(p, normalized) << where << ": NORMALIZED(" << index << ")";
            GetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_INTEGER, &p);
            EXPECT_EQ(p, integer) << where << ": INTEGER(" << index << ")";
            GetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_LONG, &p);
            EXPECT_EQ(p, isLong) << where << ": LONG(" << index << ")";
            GetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_DIVISOR, &p);
            EXPECT_EQ(p, divisor) << where << ": DIVISOR(" << index << ")";
            void* pp = nullptr;
            GetVertexAttribPointerv(index, GL_VERTEX_ATTRIB_ARRAY_POINTER, &pp);
            EXPECT_EQ(reinterpret_cast<uintptr_t>(pp), static_cast<uintptr_t>(pointer))
                << where << ": POINTER(" << index << ")";
            GetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &p);
            EXPECT_EQ(static_cast<GLuint>(p), bufferBinding) << where << ": BUFFER_BINDING(" << index << ")";
            GetVertexAttribiv(index, GL_VERTEX_ATTRIB_BINDING, &p);
            EXPECT_EQ(static_cast<GLuint>(p), binding) << where << ": BINDING(" << index << ")";
            GetVertexAttribiv(index, GL_VERTEX_ATTRIB_RELATIVE_OFFSET, &p);
            EXPECT_EQ(p, relativeOffset) << where << ": RELATIVE_OFFSET(" << index << ")";
        }
    };

    // Mirrors the CTS's VertexBindingState, initial stride 16 included.
    struct BindingState {
        explicit BindingState(GLuint bindingIndex) : index(bindingIndex) {}

        GLuint index = 0;
        GLuint buffer = 0;
        GLint offset = 0;
        GLint stride = 16;
        GLint divisor = 0;

        void Verify(const char* where) const {
            GLint p = -1;
            GetIntegeri_v(GL_VERTEX_BINDING_BUFFER, index, &p);
            EXPECT_EQ(static_cast<GLuint>(p), buffer) << where << ": VERTEX_BINDING_BUFFER(" << index << ")";
            // The CTS reads the offset through glGetInteger64i_v; that entry point's pname
            // routing is a separate defect with its own regression (see the indexed-getter
            // parity test), so the state model is pinned through the 32-bit view here.
            GetIntegeri_v(GL_VERTEX_BINDING_OFFSET, index, &p);
            EXPECT_EQ(p, offset) << where << ": VERTEX_BINDING_OFFSET(" << index << ")";
            GetIntegeri_v(GL_VERTEX_BINDING_STRIDE, index, &p);
            EXPECT_EQ(p, stride) << where << ": VERTEX_BINDING_STRIDE(" << index << ")";
            GetIntegeri_v(GL_VERTEX_BINDING_DIVISOR, index, &p);
            EXPECT_EQ(p, divisor) << where << ": VERTEX_BINDING_DIVISOR(" << index << ")";
        }
    };

    // Strict core rules only apply when the current EGL context explicitly asked for a core
    // profile; the suite's default (no current context) is relaxed. RAII so a failed
    // expectation cannot leave the context current for the rest of the binary.
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

    class VertexAttribBindingStateTest : public ::testing::Test {
    protected:
        void SetUp() override {
            MobileGL::Initialize();
            // A fresh context per case: the state model under test is cumulative, so a leftover
            // VAO binding from a neighbour would silently change what "default state" means.
            MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();
            GenVertexArrays(1, &m_vao);
            BindVertexArray(m_vao);
        }

        void TearDown() override {
            EXPECT_EQ(GetError(), GL_NO_ERROR) << "test left an unconsumed GL error behind";
        }

        GLuint CreateVbo(GLsizeiptr size) {
            GLuint vbo = 0;
            GenBuffers(1, &vbo);
            BindBuffer(GL_ARRAY_BUFFER, vbo);
            BufferData(GL_ARRAY_BUFFER, size, nullptr, GL_DYNAMIC_COPY);
            BindBuffer(GL_ARRAY_BUFFER, 0);
            return vbo;
        }

        static void DrainErrors() {
            for (int i = 0; i < 16 && GetError() != GL_NO_ERROR; ++i) {
            }
        }

        GLuint m_vao = 0;
    };

    // basic-state1's opening block: the initial per-attribute mapping and the per-binding-point
    // defaults, VERTEX_BINDING_STRIDE = 16 included. That check is the FIRST thing the CTS case
    // does, so a wrong default masked everything the case would have found after it.
    TEST_F(VertexAttribBindingStateTest, DefaultsMatchTheSpecInitialState) {
        for (GLuint i = 0; i < 16; ++i) {
            AttribState(i).Verify("defaults");
            BindingState(i).Verify("defaults");
        }
        EXPECT_EQ(GetError(), GL_NO_ERROR);
    }

    // basic-state3, verbatim: a full separate-format sequence, then a pointer call, then a
    // binding update on top of it. The legacy STRIDE/POINTER pair must stay untouched by every
    // step except the glVertexAttribPointer one, and must survive the binding update after it.
    TEST_F(VertexAttribBindingStateTest, SeparateFormatSequenceKeepsLegacyStrideAndPointerAtZero) {
        const GLuint vbo0 = CreateVbo(10000);
        const GLuint vbo1 = CreateVbo(10000);
        const GLuint vbo2 = CreateVbo(10000);
        ASSERT_EQ(GetError(), GL_NO_ERROR);

        AttribState va0(0), va2(2), va15(15);
        BindingState vb0(0), vb2(2), vb15(15);

        VertexAttribFormat(0, 2, GL_BYTE, GL_TRUE, 16);
        va0.size = 2;
        va0.type = GL_BYTE;
        va0.normalized = 1;
        va0.relativeOffset = 16;
        va0.Verify("after glVertexAttribFormat");
        // The format call says nothing about a buffer, so binding point 0 keeps its defaults -
        // stride 16 among them.
        vb0.Verify("after glVertexAttribFormat");

        VertexAttribIFormat(2, 3, GL_INT, 512);
        va2.size = 3;
        va2.type = GL_INT;
        va2.integer = 1;
        va2.relativeOffset = 512;
        va2.Verify("after glVertexAttribIFormat");
        vb2.Verify("after glVertexAttribIFormat");

        BindVertexBuffer(0, vbo0, 2048, 128);
        va0.bufferBinding = vbo0;
        vb0.buffer = vbo0;
        vb0.offset = 2048;
        vb0.stride = 128;
        va0.Verify("after glBindVertexBuffer(0)");
        vb0.Verify("after glBindVertexBuffer(0)");

        BindVertexBuffer(2, vbo2, 64, 256);
        va2.bufferBinding = vbo2;
        vb2.buffer = vbo2;
        vb2.offset = 64;
        vb2.stride = 256;
        va2.Verify("after glBindVertexBuffer(2)");
        vb2.Verify("after glBindVertexBuffer(2)");

        // Attribute 2 moves onto binding 0 and takes that binding point's buffer with it.
        VertexAttribBinding(2, 0);
        va2.binding = 0;
        va2.bufferBinding = vbo0;
        va0.Verify("after glVertexAttribBinding(2,0)");
        vb0.Verify("after glVertexAttribBinding(2,0)");
        va2.Verify("after glVertexAttribBinding(2,0)");
        vb2.Verify("after glVertexAttribBinding(2,0)");

        VertexAttribBinding(0, 15);
        va0.binding = 15;
        va0.bufferBinding = 0;
        va0.Verify("after glVertexAttribBinding(0,15)");
        vb0.Verify("after glVertexAttribBinding(0,15)");
        va15.Verify("after glVertexAttribBinding(0,15)");
        vb15.Verify("after glVertexAttribBinding(0,15)");

        BindVertexBuffer(15, vbo1, 16, 32);
        va0.bufferBinding = vbo1;
        va15.bufferBinding = vbo1;
        vb15.buffer = vbo1;
        vb15.offset = 16;
        vb15.stride = 32;
        va0.Verify("after glBindVertexBuffer(15)");
        va15.Verify("after glBindVertexBuffer(15)");
        vb15.Verify("after glBindVertexBuffer(15)");

        // The one call that IS allowed to write the legacy pair - and it also re-points the
        // attribute at its own binding point and rewrites that binding point.
        BindBuffer(GL_ARRAY_BUFFER, vbo2);
        VertexAttribPointer(0, 4, GL_UNSIGNED_BYTE, GL_FALSE, 8, reinterpret_cast<const void*>(640));
        BindBuffer(GL_ARRAY_BUFFER, 0);
        va0.size = 4;
        va0.type = GL_UNSIGNED_BYTE;
        va0.stride = 8;
        va0.pointer = 640;
        va0.relativeOffset = 0;
        va0.normalized = 0;
        va0.binding = 0;
        va0.bufferBinding = vbo2;
        vb0.buffer = vbo2;
        vb0.offset = 640;
        vb0.stride = 8;
        va2.bufferBinding = vbo2;
        va0.Verify("after glVertexAttribPointer");
        vb0.Verify("after glVertexAttribPointer");
        va2.Verify("after glVertexAttribPointer");
        va15.Verify("after glVertexAttribPointer");
        vb15.Verify("after glVertexAttribPointer");

        // ...and a binding update on top of it leaves the legacy pair exactly where the pointer
        // call left it. This is the assertion the eager resolve used to fail.
        BindVertexBuffer(0, vbo1, 80, 24);
        vb0.buffer = vbo1;
        vb0.offset = 80;
        vb0.stride = 24;
        va0.bufferBinding = vbo1;
        va2.bufferBinding = vbo1;
        va0.Verify("after the trailing glBindVertexBuffer(0)");
        vb0.Verify("after the trailing glBindVertexBuffer(0)");
        va2.Verify("after the trailing glBindVertexBuffer(0)");
        EXPECT_EQ(GetError(), GL_NO_ERROR);
    }

    // basic-state4: glVertexAttribDivisor is VertexAttribBinding(i,i) + VertexBindingDivisor(i,d),
    // and glVertexBindingDivisor reaches the attribute's own DIVISOR query either way.
    TEST_F(VertexAttribBindingStateTest, DivisorGoesThroughTheBindingPoint) {
        for (GLuint i = 0; i < 16; ++i) {
            AttribState va(i);
            BindingState vb(i);
            VertexAttribDivisor(i, i + 7);
            va.divisor = static_cast<GLint>(i + 7);
            vb.divisor = static_cast<GLint>(i + 7);
            va.Verify("after glVertexAttribDivisor");
            vb.Verify("after glVertexAttribDivisor");
        }
        for (GLuint i = 0; i < 16; ++i) {
            AttribState va(i);
            BindingState vb(i);
            VertexBindingDivisor(i, i);
            va.divisor = static_cast<GLint>(i);
            vb.divisor = static_cast<GLint>(i);
            va.Verify("after glVertexBindingDivisor");
            vb.Verify("after glVertexBindingDivisor");
        }

        // Attribute 2 moves onto binding 5 and inherits binding 5's divisor; binding 2 keeps its
        // own.
        VertexAttribBinding(2, 5);
        AttribState va5(5);
        va5.divisor = 5;
        BindingState vb5(5);
        vb5.divisor = 5;
        AttribState va2(2);
        va2.divisor = 5;
        va2.binding = 5;
        BindingState vb2(2);
        vb2.divisor = 2;
        va5.Verify("after glVertexAttribBinding(2,5)");
        vb5.Verify("after glVertexAttribBinding(2,5)");
        va2.Verify("after glVertexAttribBinding(2,5)");
        vb2.Verify("after glVertexAttribBinding(2,5)");

        // ...and glVertexAttribDivisor pulls it back onto binding 2. Guarding the write on
        // "binding already == index" left the attribute on binding 5 and threw the divisor away.
        VertexAttribDivisor(2, 23);
        va2.binding = 2;
        va2.divisor = 23;
        vb2.divisor = 23;
        va5.Verify("after glVertexAttribDivisor(2,23)");
        vb5.Verify("after glVertexAttribDivisor(2,23)");
        va2.Verify("after glVertexAttribDivisor(2,23)");
        vb2.Verify("after glVertexAttribDivisor(2,23)");
        EXPECT_EQ(GetError(), GL_NO_ERROR);
    }

    // The tail of every negative-* case: with the default vertex array bound, a core profile
    // rejects all four binding entry points.
    TEST_F(VertexAttribBindingStateTest, BindingApiRejectsTheDefaultVertexArrayInCoreProfile) {
        ScopedCoreProfileContext coreContext;
        ASSERT_FALSE(MG_State::IsRelaxedSemanticsActive());
        DrainErrors();

        BindVertexArray(0);
        ASSERT_EQ(GetError(), GL_NO_ERROR);

        BindVertexBuffer(0, 7, 0, 12);
        EXPECT_EQ(GetError(), GL_INVALID_OPERATION) << "glBindVertexBuffer";
        VertexAttribFormat(0, 4, GL_FLOAT, GL_FALSE, 0);
        EXPECT_EQ(GetError(), GL_INVALID_OPERATION) << "glVertexAttribFormat";
        VertexAttribIFormat(0, 4, GL_INT, 0);
        EXPECT_EQ(GetError(), GL_INVALID_OPERATION) << "glVertexAttribIFormat";
        VertexAttribBinding(0, 0);
        EXPECT_EQ(GetError(), GL_INVALID_OPERATION) << "glVertexAttribBinding";
        VertexBindingDivisor(0, 1);
        EXPECT_EQ(GetError(), GL_INVALID_OPERATION) << "glVertexBindingDivisor";

        BindVertexArray(m_vao);
        DrainErrors();
    }

    // ...and the relaxed default - which is what every context that never asked for a core
    // profile gets - keeps accepting them, because applications depend on it.
    TEST_F(VertexAttribBindingStateTest, BindingApiStillAcceptsTheDefaultVertexArrayWhenRelaxed) {
        ASSERT_TRUE(MG_State::IsRelaxedSemanticsActive());
        const GLuint vbo = CreateVbo(1024);
        DrainErrors();

        BindVertexArray(0);
        BindVertexBuffer(0, vbo, 0, 12);
        EXPECT_EQ(GetError(), GL_NO_ERROR) << "glBindVertexBuffer under relaxed semantics";
        VertexAttribFormat(0, 4, GL_FLOAT, GL_FALSE, 0);
        EXPECT_EQ(GetError(), GL_NO_ERROR) << "glVertexAttribFormat under relaxed semantics";
        VertexAttribBinding(0, 0);
        EXPECT_EQ(GetError(), GL_NO_ERROR) << "glVertexAttribBinding under relaxed semantics";
        VertexBindingDivisor(0, 1);
        EXPECT_EQ(GetError(), GL_NO_ERROR) << "glVertexBindingDivisor under relaxed semantics";

        BindVertexArray(m_vao);
        DrainErrors();
    }

    // MOBILEGL_RELAXED_SEMANTICS wins even on an explicit core-profile context.
    TEST_F(VertexAttribBindingStateTest, RelaxedSemanticsOverrideReopensTheDefaultVertexArray) {
        ScopedCoreProfileContext coreContext;
        const Bool saved = MG_Config::Features.RelaxedSemantics;
        MG_Config::Features.RelaxedSemantics = true;
        const GLuint vbo = CreateVbo(1024);
        DrainErrors();

        BindVertexArray(0);
        BindVertexBuffer(0, vbo, 0, 12);
        EXPECT_EQ(GetError(), GL_NO_ERROR);

        BindVertexArray(m_vao);
        MG_Config::Features.RelaxedSemantics = saved;
        DrainErrors();
    }
} // namespace
