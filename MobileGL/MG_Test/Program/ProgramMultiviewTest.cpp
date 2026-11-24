// MobileGL - MobileGL/MG_Test/Program/ProgramMultiviewTest.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// GL_OVR_multiview through the GL compile and link entry points: the vertex stage's num_views
// reaches the program (and survives a link memo hit), GL_MAX_VIEWS_OVR bounds it, the shading
// language extension and its macros exist only where the backend advertises the extension, and
// the draw-time rule compares the program with the draw framebuffer.
#include <gtest/gtest.h>

#include "Includes.h"
#include "Init.h"
#include "MG_Backend/BackendObjects.h"
#include "MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.h"
#include "MG_Impl/GLImpl/Getter/GL_Getter.h"
#include "MG_Impl/GLImpl/Program/GL_Program.h"
#include "MG_Impl/GLImpl/Texture/GL_Texture.h"
#include "MG_State/GLState/Core.h"

using namespace MobileGL;
using namespace MobileGL::MG_Impl::GLImpl;

namespace {
    // A backend that claims a type, so the compile environment gates extensions against its list.
    class MultiviewProgramBackend final : public MG_Backend::BackendObject {
    public:
        MultiviewProgramBackend(Int maxViews, Vector<GLExtension> extensions) {
            m_params.MaxViews = maxViews;
            m_params.MaxArrayTextureLayers = 8;
            m_info.RendererGLInfo.Extensions = Move(extensions);
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
        const MG_Backend::DynamicBackendParameters& GetDynamicParameters() const override { return m_params; }
        BackendType GetBackendType() const override { return BackendType::DirectGLES; }

    private:
        MG_Backend::DynamicBackendParameters m_params{};
        RendererInfo m_info{};
    };

    class ScopedProgramBackend {
    public:
        ScopedProgramBackend(Int maxViews, Vector<GLExtension> extensions)
            : m_previous(Move(MG_Backend::pActiveBackendObject)) {
            MG_Backend::pActiveBackendObject = MakeUnique<MultiviewProgramBackend>(maxViews, Move(extensions));
            // The context re-captures its compile environment when the backend's address changes,
            // and a freshly allocated mock can land on the address the previous one had.
            MG_State::pGLContext->InvalidateCompileEnv();
        }
        ~ScopedProgramBackend() {
            MG_Backend::pActiveBackendObject = Move(m_previous);
            MG_State::pGLContext->InvalidateCompileEnv();
        }

    private:
        UniquePtr<MG_Backend::BackendObject> m_previous;
    };

    const Vector<GLExtension> kBothExtensions = {E_GL_OVR_multiview, E_GL_OVR_multiview2};

    GLuint CompiledShader(GLenum type, const char* source, GLint& status) {
        const GLuint shader = CreateShader(type);
        ShaderSource(shader, 1, &source, nullptr);
        CompileShader(shader);
        GetShaderiv(shader, GL_COMPILE_STATUS, &status);
        return shader;
    }

    Bool Compiles(GLenum type, const char* source) {
        GLint status = GL_FALSE;
        CompiledShader(type, source, status);
        return status == GL_TRUE;
    }

    // The linked program, or 0 when it did not link.
    GLuint LinkedProgram(const char* vertex, const char* fragment) {
        GLint status = GL_FALSE;
        const GLuint vs = CompiledShader(GL_VERTEX_SHADER, vertex, status);
        EXPECT_EQ(status, GL_TRUE);
        const GLuint fs = CompiledShader(GL_FRAGMENT_SHADER, fragment, status);
        EXPECT_EQ(status, GL_TRUE);
        const GLuint program = CreateProgram();
        AttachShader(program, vs);
        AttachShader(program, fs);
        LinkProgram(program);
        GetProgramiv(program, GL_LINK_STATUS, &status);
        return status == GL_TRUE ? program : 0;
    }

    String MultiviewVertex(Int numViews) {
        return std::format("#version 330 core\n"
                           "#extension GL_OVR_multiview2 : require\n"
                           "layout(num_views = {}) in;\n"
                           "layout(location = 0) in vec2 pos;\n"
                           "void main() {{ gl_Position = vec4(pos.x + float(gl_ViewID_OVR), pos.y, 0.0, 1.0); }}\n",
                           numViews);
    }

    constexpr const char* kFragment = "#version 330 core\n"
                                      "out vec4 color;\n"
                                      "void main() { color = vec4(1.0); }\n";
} // namespace

class ProgramMultiviewTest : public ::testing::Test {
protected:
    void SetUp() override {
        MobileGL::Initialize();
        for (Int drained = 0; drained < 16 && GetError() != GL_NO_ERROR; ++drained) {}
    }
    void TearDown() override { EXPECT_EQ(GetError(), GL_NO_ERROR) << "test left an unconsumed GL error behind"; }
};

TEST_F(ProgramMultiviewTest, TheVertexStagesNumViewsReachesTheProgramAndSurvivesAMemoHit) {
    const ScopedProgramBackend backend(4, kBothExtensions);
    const String vertex = MultiviewVertex(3);
    for (Int link = 0; link < 2; ++link) {
        const GLuint program = LinkedProgram(vertex.c_str(), kFragment);
        ASSERT_NE(program, 0u) << "link " << link;
        const auto programObject = MG_State::pGLContext->GetProgramObject(program);
        ASSERT_NE(programObject, nullptr);
        EXPECT_EQ(programObject->GetMultiviewViewCount(), 3) << "link " << link;
    }

    const GLuint plain = LinkedProgram("#version 330 core\nvoid main() { gl_Position = vec4(0.0); }\n", kFragment);
    ASSERT_NE(plain, 0u);
    EXPECT_EQ(MG_State::pGLContext->GetProgramObject(plain)->GetMultiviewViewCount(), 1);
}

TEST_F(ProgramMultiviewTest, MoreViewsThanTheBackendHasFailsTheLink) {
    const ScopedProgramBackend backend(4, kBothExtensions);
    const String vertex = MultiviewVertex(5);
    EXPECT_EQ(LinkedProgram(vertex.c_str(), kFragment), 0u);
}

TEST_F(ProgramMultiviewTest, TheShadingLanguageExtensionFollowsTheAdvertisement) {
    constexpr const char* kRequireFirst = "#version 330 core\n#extension GL_OVR_multiview : require\n"
                                          "void main() { gl_Position = vec4(float(gl_ViewID_OVR)); }\n";
    constexpr const char* kRequireSecond = "#version 330 core\n#extension GL_OVR_multiview2 : require\n"
                                           "void main() { gl_Position = vec4(float(gl_ViewID_OVR)); }\n";
    constexpr const char* kMacroAbsent = "#version 330 core\n#if defined(GL_OVR_multiview) || "
                                         "defined(GL_OVR_multiview2)\n#error the macro is defined\n#endif\n"
                                         "void main() { gl_Position = vec4(0.0); }\n";
    constexpr const char* kMacroPresent = "#version 330 core\n#ifndef GL_OVR_multiview\n#error the macro is "
                                          "missing\n#endif\nvoid main() { gl_Position = vec4(0.0); }\n";
    {
        const ScopedProgramBackend backend(0, {});
        EXPECT_FALSE(Compiles(GL_VERTEX_SHADER, kRequireFirst));
        EXPECT_FALSE(Compiles(GL_VERTEX_SHADER, kRequireSecond));
        EXPECT_TRUE(Compiles(GL_VERTEX_SHADER, kMacroAbsent));
    }
    {
        const ScopedProgramBackend backend(4, {E_GL_OVR_multiview});
        EXPECT_TRUE(Compiles(GL_VERTEX_SHADER, kRequireFirst));
        EXPECT_FALSE(Compiles(GL_VERTEX_SHADER, kRequireSecond));
        EXPECT_TRUE(Compiles(GL_VERTEX_SHADER, kMacroPresent));
    }
    {
        const ScopedProgramBackend backend(4, kBothExtensions);
        EXPECT_TRUE(Compiles(GL_VERTEX_SHADER, kRequireFirst));
        EXPECT_TRUE(Compiles(GL_VERTEX_SHADER, kRequireSecond));
        EXPECT_TRUE(Compiles(GL_VERTEX_SHADER, kMacroPresent));
    }
}

TEST_F(ProgramMultiviewTest, ADrawNeedsTheProgramsViewCountInTheDrawFramebuffer) {
    const ScopedProgramBackend backend(4, kBothExtensions);
    const String vertex = MultiviewVertex(2);
    const GLuint multiviewProgram = LinkedProgram(vertex.c_str(), kFragment);
    const GLuint plainProgram =
        LinkedProgram("#version 330 core\nvoid main() { gl_Position = vec4(0.0); }\n", kFragment);
    ASSERT_NE(multiviewProgram, 0u);
    ASSERT_NE(plainProgram, 0u);
    const auto& multiview = *MG_State::pGLContext->GetProgramObject(multiviewProgram);
    const auto& plain = *MG_State::pGLContext->GetProgramObject(plainProgram);

    auto& drawSlot = MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw);
    const auto previous = drawSlot.GetBoundObject();

    // The default framebuffer has one view.
    drawSlot.Bind(MG_State::pGLContext->GetFramebufferObject(0));
    EXPECT_TRUE(ValidateMultiviewDraw(plain, "test"));
    EXPECT_FALSE(ValidateMultiviewDraw(multiview, "test"));
    EXPECT_EQ(GetError(), static_cast<GLenum>(GL_INVALID_OPERATION));

    GLuint texture = 0;
    CreateTextures(GL_TEXTURE_2D_ARRAY, 1, &texture);
    TextureStorage3D(texture, 1, GL_RGBA8, 4, 4, 2);
    GLuint framebuffer = 0;
    CreateFramebuffers(1, &framebuffer);
    NamedFramebufferTextureMultiviewOVR(framebuffer, GL_COLOR_ATTACHMENT0, texture, 0, 0, 2);
    ASSERT_EQ(GetError(), static_cast<GLenum>(GL_NO_ERROR));
    drawSlot.Bind(MG_State::pGLContext->GetFramebufferObject(framebuffer));
    EXPECT_TRUE(ValidateMultiviewDraw(multiview, "test"));
    EXPECT_FALSE(ValidateMultiviewDraw(plain, "test"));
    EXPECT_EQ(GetError(), static_cast<GLenum>(GL_INVALID_OPERATION));

    drawSlot.Bind(previous);
}
