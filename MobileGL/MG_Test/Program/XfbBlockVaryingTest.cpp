// MobileGL - MobileGL/MG_Test/Program/XfbBlockVaryingTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// Transform-feedback capture of a member of an output interface block.
//
// GL 4.6 core 11.1.2.1 names such a varying "<BLOCK name>.<member>" - the block's TYPE
// name, never the instance name - which is exactly what KHR-GL4x.vertex_attrib_binding
// (gl4cVertexAttribBindingTests.cpp:419-437, `out StageData { vec4 attrib[16]; } vs_out;`
// captured as "StageData.attrib[0]".."[15]") relies on. The resolver used to match the
// requested name against glslang's linker-object symbol name, which for a block is the
// INSTANCE ("vs_out"), so every one of those captures came back unresolved and the link
// failed with "is not an output of the vertex stage" + GL_INVALID_VALUE.
//
// GPU-free: everything asserted here is a property of the link, not of any driver.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "Includes.h"
#include "Init.h"
#include "MG_Impl/GLImpl/Getter/GL_Getter.h"
#include "MG_Impl/GLImpl/Program/GL_Program.h"
#include "MG_State/GLState/Core.h"

using namespace MobileGL;
using namespace MobileGL::MG_Impl::GLImpl;

namespace {
    class XfbBlockVaryingTest : public ::testing::Test {
    protected:
        void SetUp() override { MobileGL::Initialize(); }
    };

    GLuint MakeVsOnlyProgram(const char* vs) {
        const GLuint program = CreateProgram();
        const GLuint shader = CreateShader(GL_VERTEX_SHADER);
        ShaderSource(shader, 1, &vs, nullptr);
        CompileShader(shader);
        GLint compiled = GL_FALSE;
        GetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        EXPECT_EQ(compiled, GL_TRUE) << [&] {
            char log[4096] = "";
            GetShaderInfoLog(shader, sizeof(log), nullptr, log);
            return std::string(log);
        }();
        AttachShader(program, shader);
        return program;
    }

    std::string LinkLog(GLuint program) {
        char log[4096] = "";
        GetProgramInfoLog(program, sizeof(log), nullptr, log);
        return std::string(log);
    }

    GLint Programiv(GLuint program, GLenum pname) {
        GLint value = -1;
        GetProgramiv(program, pname, &value);
        return value;
    }

    struct VaryingRecord {
        std::string name;
        GLsizei size = 0;
        GLenum type = 0;
    };

    VaryingRecord Varying(GLuint program, GLuint index) {
        VaryingRecord record;
        GLchar buffer[256] = {'\0'};
        GLsizei length = 0;
        GetTransformFeedbackVarying(program, index, sizeof(buffer), &length, &record.size, &record.type, buffer);
        record.name.assign(buffer, buffer + (length < 0 ? 0 : length));
        return record;
    }

    void ClearErrors() {
        for (int i = 0; i < 32 && GetError() != GL_NO_ERROR; ++i) {
        }
    }

    // The CTS shader, narrowed to two elements so the expectations stay readable.
    const char* kNamedBlockVs = R"(#version 430 core
layout(location = 0) in vec4 vs_in_attrib[2];
out StageData {
  vec4 attrib[2];
} vs_out;
void main() {
  for (int i = 0; i < vs_in_attrib.length(); ++i) {
    vs_out.attrib[i] = vs_in_attrib[i];
  }
}
)";

    TEST_F(XfbBlockVaryingTest, CapturesBlockMemberElementsByBlockTypeName) {
        ClearErrors();
        const GLuint program = MakeVsOnlyProgram(kNamedBlockVs);
        const GLchar* const varyings[2] = {"StageData.attrib[0]", "StageData.attrib[1]"};
        TransformFeedbackVaryings(program, 2, varyings, GL_INTERLEAVED_ATTRIBS);
        LinkProgram(program);

        ASSERT_EQ(Programiv(program, GL_LINK_STATUS), GL_TRUE) << LinkLog(program);
        EXPECT_EQ(GetError(), GL_NO_ERROR);
        EXPECT_EQ(Programiv(program, GL_TRANSFORM_FEEDBACK_VARYINGS), 2);
        EXPECT_EQ(Programiv(program, GL_TRANSFORM_FEEDBACK_BUFFER_MODE), GL_INTERLEAVED_ATTRIBS);

        for (GLuint i = 0; i < 2; ++i) {
            const VaryingRecord record = Varying(program, i);
            EXPECT_EQ(record.name, std::string("StageData.attrib[") + std::to_string(i) + "]");
            // One element of the member array, not the whole array.
            EXPECT_EQ(record.size, 1) << "index " << i;
            EXPECT_EQ(record.type, static_cast<GLenum>(GL_FLOAT_VEC4)) << "index " << i;
        }
    }

    // The whole member, no subscript: the array size has to survive.
    TEST_F(XfbBlockVaryingTest, CapturesAWholeBlockMemberArray) {
        ClearErrors();
        const GLuint program = MakeVsOnlyProgram(kNamedBlockVs);
        const GLchar* const varyings[1] = {"StageData.attrib"};
        TransformFeedbackVaryings(program, 1, varyings, GL_INTERLEAVED_ATTRIBS);
        LinkProgram(program);

        ASSERT_EQ(Programiv(program, GL_LINK_STATUS), GL_TRUE) << LinkLog(program);
        const VaryingRecord record = Varying(program, 0);
        EXPECT_EQ(record.name, "StageData.attrib");
        EXPECT_EQ(record.size, 2);
        EXPECT_EQ(record.type, static_cast<GLenum>(GL_FLOAT_VEC4));
    }

    // Members of an anonymous instance are named the same way - the block name is still
    // what identifies them, and there is no instance name to fall back on.
    TEST_F(XfbBlockVaryingTest, CapturesAnonymousInstanceBlockMember) {
        ClearErrors();
        const GLuint program = MakeVsOnlyProgram(R"(#version 430 core
layout(location = 0) in vec4 vs_in_attrib;
out StageData {
  vec4 color;
  vec2 uv;
};
void main() {
  color = vs_in_attrib;
  uv = vs_in_attrib.xy;
}
)");
        const GLchar* const varyings[2] = {"StageData.color", "StageData.uv"};
        TransformFeedbackVaryings(program, 2, varyings, GL_INTERLEAVED_ATTRIBS);
        LinkProgram(program);

        ASSERT_EQ(Programiv(program, GL_LINK_STATUS), GL_TRUE) << LinkLog(program);
        EXPECT_EQ(Varying(program, 0).type, static_cast<GLenum>(GL_FLOAT_VEC4));
        EXPECT_EQ(Varying(program, 1).type, static_cast<GLenum>(GL_FLOAT_VEC2));
    }

    // The instance-qualified spelling is not what the spec asks for, but it is what a lot of
    // application code writes; resolving it too costs nothing and keeps those links alive.
    TEST_F(XfbBlockVaryingTest, AlsoAcceptsTheInstanceQualifiedSpelling) {
        ClearErrors();
        const GLuint program = MakeVsOnlyProgram(kNamedBlockVs);
        const GLchar* const varyings[1] = {"vs_out.attrib[1]"};
        TransformFeedbackVaryings(program, 1, varyings, GL_INTERLEAVED_ATTRIBS);
        LinkProgram(program);

        ASSERT_EQ(Programiv(program, GL_LINK_STATUS), GL_TRUE) << LinkLog(program);
        EXPECT_EQ(Varying(program, 0).size, 1);
        EXPECT_EQ(Varying(program, 0).type, static_cast<GLenum>(GL_FLOAT_VEC4));
    }

    // A dotted path that resolves to nothing must still fail the link, and say so - the
    // fix must not turn "unknown member" into a silently dropped capture.
    TEST_F(XfbBlockVaryingTest, RejectsAnUnknownBlockMember) {
        ClearErrors();
        const GLuint program = MakeVsOnlyProgram(kNamedBlockVs);
        const GLchar* const varyings[1] = {"StageData.missing"};
        TransformFeedbackVaryings(program, 1, varyings, GL_INTERLEAVED_ATTRIBS);
        LinkProgram(program);

        EXPECT_EQ(Programiv(program, GL_LINK_STATUS), GL_FALSE);
        EXPECT_NE(LinkLog(program).find("StageData.missing"), std::string::npos) << LinkLog(program);
    }

    TEST_F(XfbBlockVaryingTest, RejectsAnUnknownBlock) {
        ClearErrors();
        const GLuint program = MakeVsOnlyProgram(kNamedBlockVs);
        const GLchar* const varyings[1] = {"NoSuchBlock.attrib[0]"};
        TransformFeedbackVaryings(program, 1, varyings, GL_INTERLEAVED_ATTRIBS);
        LinkProgram(program);

        EXPECT_EQ(Programiv(program, GL_LINK_STATUS), GL_FALSE);
    }

    // Plain (non-block) outputs must keep resolving exactly as before.
    TEST_F(XfbBlockVaryingTest, StillResolvesPlainOutputs) {
        ClearErrors();
        const GLuint program = MakeVsOnlyProgram(R"(#version 430 core
layout(location = 0) in vec4 vs_in_attrib;
out vec4 plain[2];
out vec3 single;
void main() {
  plain[0] = vs_in_attrib;
  plain[1] = vs_in_attrib;
  single = vs_in_attrib.xyz;
}
)");
        const GLchar* const varyings[3] = {"plain[1]", "single", "gl_Position"};
        TransformFeedbackVaryings(program, 3, varyings, GL_INTERLEAVED_ATTRIBS);
        LinkProgram(program);

        ASSERT_EQ(Programiv(program, GL_LINK_STATUS), GL_TRUE) << LinkLog(program);
        EXPECT_EQ(Varying(program, 0).size, 1);
        EXPECT_EQ(Varying(program, 0).type, static_cast<GLenum>(GL_FLOAT_VEC4));
        EXPECT_EQ(Varying(program, 1).type, static_cast<GLenum>(GL_FLOAT_VEC3));
        EXPECT_EQ(Varying(program, 2).type, static_cast<GLenum>(GL_FLOAT_VEC4));
    }
} // namespace
