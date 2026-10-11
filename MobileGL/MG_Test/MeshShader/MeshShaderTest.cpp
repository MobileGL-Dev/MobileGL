// MobileGL - MobileGL/MG_Test/MeshShader/MeshShader.cpp
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
#include "MG_Impl/GLImpl/Program/GL_Program.h"

#include <MG_Impl/GLImpl/Buffer/GL_Buffer.h>
#include <MG_Impl/GLImpl/Getter/GL_Getter.h>
#include <MG_Impl/GLImpl/VertexArray/GL_VertexArray.h>
#include <MG_Impl/GLImpl/VertexArray/Validators.h>
#include <MG_State/GLState/Core.h>

using namespace MobileGL;

class MeshShaderTest : public ::testing::Test {
protected:
    void SetUp() override { MobileGL::Initialize(); }

    void TearDown() override {
        EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR) << "test left an unconsumed GL error behind";
    }
};

TEST_F(MeshShaderTest, IsHaveExt) {
    const auto* extensionsString = reinterpret_cast<const char*>(MG_Impl::GLImpl::GetString(GL_EXTENSIONS));
    ASSERT_NE(extensionsString, nullptr);
    // std::cout << extensionsString << std::endl;
    const Bool isHaveExt = String(extensionsString).find("GL_NV_mesh_shader") != String::npos;
    ASSERT_EQ(isHaveExt, true);
}

TEST_F(MeshShaderTest, TaskShaderCompile) {
    const GLuint shader = MG_Impl::GLImpl::CreateShader(GL_TASK_SHADER_NV);
    const char* src = R"GLSL(
        #version 460
        #extension GL_NV_mesh_shader : require

        layout(local_size_x = 32) in;

        layout(location = 0) uniform vec4 uParams;   // x = grid, y = aspect, z = time

        void main()
        {
            if (gl_LocalInvocationID.x != 0u)
                return;

            uint grid = uint(uParams.x);

            gl_TaskCountNV = grid * grid;
        }
    )GLSL";
    MG_Impl::GLImpl::ShaderSource(shader, 1, &src, nullptr);
    MG_Impl::GLImpl::CompileShader(shader);

    GLint ok = GL_FALSE;
    MG_Impl::GLImpl::GetShaderiv(shader, GL_COMPILE_STATUS, &ok);

    ASSERT_EQ(ok, GL_TRUE);
}
