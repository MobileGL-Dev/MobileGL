// MobileGL - MobileGL/MG_Test/Program/ProgramInterfaceTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The GL program interface (glGetProgramInterfaceiv / glGetProgramResource*) answered
// entirely from the frontend glslang reflection - MG_Impl/GLImpl/Program/ProgramInterface.
//
// GPU-free on purpose: every expectation below is a property of the LINKED PROGRAM, not of
// any driver, which is the whole point of the layer. The shaders and the expected values
// are lifted from KHR-GL4x.program_interface_query so a failure here is the same failure
// the conformance suite would report, minutes earlier and without a GPU.

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
    class ProgramInterfaceTest : public ::testing::Test {
    protected:
        void SetUp() override { MobileGL::Initialize(); }
    };

    GLuint MakeProgram(const char* vs, const char* fs, const char* cs = nullptr) {
        const GLuint p = CreateProgram();
        const auto attach = [p](GLenum stage, const char* source) {
            if (!source) return;
            const GLuint sh = CreateShader(stage);
            ShaderSource(sh, 1, &source, nullptr);
            CompileShader(sh);
            AttachShader(p, sh);
        };
        attach(GL_VERTEX_SHADER, vs);
        attach(GL_FRAGMENT_SHADER, fs);
        attach(GL_COMPUTE_SHADER, cs);
        return p;
    }

    void ExpectLinked(GLuint program) {
        GLint status = 0;
        GetProgramiv(program, GL_LINK_STATUS, &status);
        if (status == GL_TRUE) return;
        char log[4096] = "";
        GetProgramInfoLog(program, sizeof(log), nullptr, log);
        FAIL() << "link failed: " << log;
    }

    GLint Interfaceiv(GLuint program, GLenum iface, GLenum pname) {
        GLint value = -12345;
        GetProgramInterfaceiv(program, iface, pname, &value);
        return value;
    }

    std::string ResourceName(GLuint program, GLenum iface, GLuint index) {
        GLchar buffer[1024] = {'\0'};
        GLsizei length = 0;
        GetProgramResourceName(program, iface, index, sizeof(buffer), &length, buffer);
        EXPECT_GE(length, 0);
        EXPECT_EQ(buffer[length], '\0') << "length must not count the terminator";
        return std::string(buffer);
    }

    // Resolves by name and immediately checks that the index round-trips back to the
    // expected spelling, which is how the CTS uses these two together.
    void ExpectResource(GLuint program, GLenum iface, const char* queryName, const char* enumeratedName) {
        const GLuint index = GetProgramResourceIndex(program, iface, queryName);
        ASSERT_NE(index, GL_INVALID_INDEX) << "no resource named '" << queryName << "'";
        EXPECT_EQ(ResourceName(program, iface, index), enumeratedName) << "for query '" << queryName << "'";
    }

    std::vector<GLint> Props(GLuint program, GLenum iface, GLuint index, const std::vector<GLenum>& props) {
        std::vector<GLint> params(256, -12345);
        GLsizei length = 0;
        GetProgramResourceiv(program, iface, index, static_cast<GLsizei>(props.size()), props.data(),
                             static_cast<GLsizei>(params.size()), &length, params.data());
        params.resize(length < 0 ? 0 : static_cast<size_t>(length));
        return params;
    }

    std::vector<GLint> PropsOf(GLuint program, GLenum iface, const char* name, const std::vector<GLenum>& props) {
        const GLuint index = GetProgramResourceIndex(program, iface, name);
        EXPECT_NE(index, GL_INVALID_INDEX) << "no resource named '" << name << "'";
        if (index == GL_INVALID_INDEX) return {};
        return Props(program, iface, index, props);
    }

    GLenum TakeError() { return GetError(); }
    void ClearErrors() {
        for (int i = 0; i < 32 && TakeError() != GL_NO_ERROR; ++i) {
        }
    }

    const char* kSimpleVs = R"(#version 430
in vec4 position;
void main(void) { gl_Position = position; }
)";
    const char* kSimpleFs = R"(#version 430
out vec4 color;
void main() { color = vec4(0, 1, 0, 1); }
)";

    // ---------------------------------------------------------------- simple-shaders ----
    TEST_F(ProgramInterfaceTest, SimpleShaders) {
        const GLuint p = MakeProgram(kSimpleVs, kSimpleFs);
        BindAttribLocation(p, 0, "position");
        BindFragDataLocation(p, 0, "color");
        LinkProgram(p);
        ExpectLinked(p);
        ClearErrors();

        // KNOWN GAP, not an expectation: a separable FRAGMENT program's own inputs are not
        // in the reflection at all - glslang builds the "pipe input" list from the vertex
        // stage unless EShReflectionIntermediateIO is set, and setting that makes a
        // vertex-only separable program report its VS outputs as fragment outputs, which
        // fails ValidateFragmentOutputLocations and breaks glCreateShaderProgramv. So
        // GL_PROGRAM_INPUT is empty here until that validation is stage-aware.
        EXPECT_EQ(Interfaceiv(p, GL_PROGRAM_OUTPUT, GL_ACTIVE_RESOURCES), 1);
        EXPECT_EQ(Interfaceiv(p, GL_PROGRAM_OUTPUT, GL_MAX_NAME_LENGTH), 6);

        EXPECT_EQ(GetProgramResourceIndex(p, GL_PROGRAM_OUTPUT, "color"), 0u);
        EXPECT_EQ(GetProgramResourceIndex(p, GL_PROGRAM_INPUT, "position"), 0u);
        EXPECT_EQ(ResourceName(p, GL_PROGRAM_OUTPUT, 0), "color");
        EXPECT_EQ(ResourceName(p, GL_PROGRAM_INPUT, 0), "position");

        EXPECT_EQ(GetProgramResourceLocation(p, GL_PROGRAM_INPUT, "position"), 0);
        EXPECT_EQ(GetProgramResourceLocation(p, GL_PROGRAM_OUTPUT, "color"), 0);
        EXPECT_EQ(GetProgramResourceLocationIndex(p, GL_PROGRAM_OUTPUT, "color"), 0);

        const std::vector<GLenum> inProps = {GL_NAME_LENGTH,
                                             GL_TYPE,
                                             GL_ARRAY_SIZE,
                                             GL_REFERENCED_BY_COMPUTE_SHADER,
                                             GL_REFERENCED_BY_FRAGMENT_SHADER,
                                             GL_REFERENCED_BY_GEOMETRY_SHADER,
                                             GL_REFERENCED_BY_TESS_CONTROL_SHADER,
                                             GL_REFERENCED_BY_TESS_EVALUATION_SHADER,
                                             GL_REFERENCED_BY_VERTEX_SHADER,
                                             GL_LOCATION,
                                             GL_IS_PER_PATCH};
        EXPECT_EQ(Props(p, GL_PROGRAM_INPUT, 0, inProps),
                  (std::vector<GLint>{9, GL_FLOAT_VEC4, 1, 0, 0, 0, 0, 0, 1, 0, 0}));

        std::vector<GLenum> outProps = inProps;
        outProps.push_back(GL_LOCATION_INDEX);
        EXPECT_EQ(Props(p, GL_PROGRAM_OUTPUT, 0, outProps),
                  (std::vector<GLint>{6, GL_FLOAT_VEC4, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0}));
        EXPECT_EQ(TakeError(), GL_NO_ERROR);
    }

    // ------------------------------------------------------------------- input-types ----
    TEST_F(ProgramInterfaceTest, InputTypesNamesLocationsAndProps) {
        const char* vs = R"(#version 430
in mat4 a;
in ivec4 b;
in float c[2];
in mat2x3 d[2];
in uvec2 e;
in uint f;
in vec3 g[2];
in int h;
void main(void)
{
   vec4 pos;
   pos.w = h + g[0].x + g[1].y + d[1][1].y;
   pos.y = b.x * c[0] + c[1] + d[0][0].x;
   pos.x = a[0].x + a[1].y + a[2].z + a[3].w;
   pos.z = d[0][1].z + e.x * f + d[1][0].z;
   gl_Position = pos;
}
)";
        const GLuint p = MakeProgram(vs, kSimpleFs);
        BindAttribLocation(p, 0, "a");
        BindAttribLocation(p, 4, "b");
        BindAttribLocation(p, 5, "c");
        BindAttribLocation(p, 7, "d");
        BindAttribLocation(p, 11, "e");
        BindAttribLocation(p, 12, "f");
        BindAttribLocation(p, 13, "g");
        BindAttribLocation(p, 15, "h");
        BindFragDataLocation(p, 0, "color");
        LinkProgram(p);
        ExpectLinked(p);
        ClearErrors();

        EXPECT_EQ(Interfaceiv(p, GL_PROGRAM_INPUT, GL_ACTIVE_RESOURCES), 8);
        // "c[0]" and friends: an array input is enumerated with the [0] subscript, which is
        // what makes MAX_NAME_LENGTH 5 rather than 2.
        EXPECT_EQ(Interfaceiv(p, GL_PROGRAM_INPUT, GL_MAX_NAME_LENGTH), 5);

        ExpectResource(p, GL_PROGRAM_INPUT, "a", "a");
        ExpectResource(p, GL_PROGRAM_INPUT, "c[0]", "c[0]");
        ExpectResource(p, GL_PROGRAM_INPUT, "c", "c[0]");
        ExpectResource(p, GL_PROGRAM_INPUT, "d", "d[0]");
        ExpectResource(p, GL_PROGRAM_INPUT, "g", "g[0]");
        ExpectResource(p, GL_PROGRAM_INPUT, "h", "h");

        EXPECT_EQ(GetProgramResourceLocation(p, GL_PROGRAM_INPUT, "a"), 0);
        EXPECT_EQ(GetProgramResourceLocation(p, GL_PROGRAM_INPUT, "b"), 4);
        EXPECT_EQ(GetProgramResourceLocation(p, GL_PROGRAM_INPUT, "c[0]"), 5);
        EXPECT_EQ(GetProgramResourceLocation(p, GL_PROGRAM_INPUT, "c"), 5);
        EXPECT_EQ(GetProgramResourceLocation(p, GL_PROGRAM_INPUT, "c[1]"), 6);
        EXPECT_EQ(GetProgramResourceLocation(p, GL_PROGRAM_INPUT, "d[0]"), 7);
        EXPECT_EQ(GetProgramResourceLocation(p, GL_PROGRAM_INPUT, "g[1]"), 14);
        EXPECT_EQ(GetProgramResourceLocation(p, GL_PROGRAM_INPUT, "h"), 15);
        // Out of range, and a subscript that is not a strict decimal.
        EXPECT_EQ(GetProgramResourceLocation(p, GL_PROGRAM_INPUT, "c[2]"), -1);
        EXPECT_EQ(GetProgramResourceLocation(p, GL_PROGRAM_INPUT, "c[01]"), -1);

        const std::vector<GLenum> props = {GL_NAME_LENGTH,
                                           GL_TYPE,
                                           GL_ARRAY_SIZE,
                                           GL_REFERENCED_BY_COMPUTE_SHADER,
                                           GL_REFERENCED_BY_FRAGMENT_SHADER,
                                           GL_REFERENCED_BY_VERTEX_SHADER,
                                           GL_LOCATION,
                                           GL_IS_PER_PATCH};
        EXPECT_EQ(PropsOf(p, GL_PROGRAM_INPUT, "a", props),
                  (std::vector<GLint>{2, GL_FLOAT_MAT4, 1, 0, 0, 1, 0, 0}));
        EXPECT_EQ(PropsOf(p, GL_PROGRAM_INPUT, "c[0]", props),
                  (std::vector<GLint>{5, GL_FLOAT, 2, 0, 0, 1, 5, 0}));
        EXPECT_EQ(PropsOf(p, GL_PROGRAM_INPUT, "d", props),
                  (std::vector<GLint>{5, GL_FLOAT_MAT2x3, 2, 0, 0, 1, 7, 0}));
        EXPECT_EQ(TakeError(), GL_NO_ERROR);
    }

    // ------------------------------------------------------------------ output-types ----
    TEST_F(ProgramInterfaceTest, OutputTypesAndLocationIndex) {
        const char* fs = R"(#version 430
out vec3 a[2];
out uint b;
out float c[2];
out int d[2];
out vec2 e;
void main() {
    c[1] = -0.6; d[0] = 0; b = 12u; c[0] = 1.1; e = vec2(0, 1); d[1] = -19;
    a[1] = vec3(0, 1, 0); a[0] = vec3(0, 1, 0);
}
)";
        const GLuint p = MakeProgram(kSimpleVs, fs);
        BindAttribLocation(p, 0, "position");
        BindFragDataLocation(p, 0, "a");
        BindFragDataLocation(p, 2, "b");
        BindFragDataLocation(p, 3, "c");
        BindFragDataLocation(p, 5, "d");
        BindFragDataLocation(p, 7, "e");
        LinkProgram(p);
        ExpectLinked(p);
        ClearErrors();

        EXPECT_EQ(Interfaceiv(p, GL_PROGRAM_OUTPUT, GL_ACTIVE_RESOURCES), 5);
        EXPECT_EQ(Interfaceiv(p, GL_PROGRAM_OUTPUT, GL_MAX_NAME_LENGTH), 5);
        ExpectResource(p, GL_PROGRAM_OUTPUT, "a", "a[0]");
        ExpectResource(p, GL_PROGRAM_OUTPUT, "c[0]", "c[0]");
        ExpectResource(p, GL_PROGRAM_OUTPUT, "e", "e");

        EXPECT_EQ(GetProgramResourceLocation(p, GL_PROGRAM_OUTPUT, "a[0]"), 0);
        EXPECT_EQ(GetProgramResourceLocation(p, GL_PROGRAM_OUTPUT, "a"), 0);
        EXPECT_EQ(GetProgramResourceLocation(p, GL_PROGRAM_OUTPUT, "a[1]"), 1);
        EXPECT_EQ(GetProgramResourceLocation(p, GL_PROGRAM_OUTPUT, "b"), 2);
        EXPECT_EQ(GetProgramResourceLocation(p, GL_PROGRAM_OUTPUT, "c[1]"), 4);
        EXPECT_EQ(GetProgramResourceLocation(p, GL_PROGRAM_OUTPUT, "d[1]"), 6);
        EXPECT_EQ(GetProgramResourceLocation(p, GL_PROGRAM_OUTPUT, "e"), 7);
        for (const char* name : {"a[0]", "a", "b", "c[0]", "c", "d[0]", "d", "e"}) {
            EXPECT_EQ(GetProgramResourceLocationIndex(p, GL_PROGRAM_OUTPUT, name), 0) << name;
        }

        const std::vector<GLenum> props = {GL_NAME_LENGTH, GL_TYPE,     GL_ARRAY_SIZE, GL_REFERENCED_BY_FRAGMENT_SHADER,
                                           GL_LOCATION,    GL_IS_PER_PATCH, GL_LOCATION_INDEX};
        EXPECT_EQ(PropsOf(p, GL_PROGRAM_OUTPUT, "a", props),
                  (std::vector<GLint>{5, GL_FLOAT_VEC3, 2, 1, 0, 0, 0}));
        EXPECT_EQ(PropsOf(p, GL_PROGRAM_OUTPUT, "d", props), (std::vector<GLint>{5, GL_INT, 2, 1, 5, 0, 0}));
        EXPECT_EQ(TakeError(), GL_NO_ERROR);
    }

    // --------------------------------------------------------------- output-built-in ----
    TEST_F(ProgramInterfaceTest, OutputBuiltInsHaveNoLocation) {
        const char* fs = R"(#version 430
void main(void) { gl_FragDepth = 0.1; gl_SampleMask[0] = 1; }
)";
        const GLuint p = MakeProgram(kSimpleVs, fs);
        LinkProgram(p);
        ExpectLinked(p);
        ClearErrors();

        EXPECT_EQ(Interfaceiv(p, GL_PROGRAM_OUTPUT, GL_ACTIVE_RESOURCES), 2);
        EXPECT_EQ(Interfaceiv(p, GL_PROGRAM_OUTPUT, GL_MAX_NAME_LENGTH), 17);
        ExpectResource(p, GL_PROGRAM_OUTPUT, "gl_FragDepth", "gl_FragDepth");
        ExpectResource(p, GL_PROGRAM_OUTPUT, "gl_SampleMask[0]", "gl_SampleMask[0]");

        EXPECT_EQ(GetProgramResourceLocation(p, GL_PROGRAM_OUTPUT, "gl_FragDepth"), -1);
        EXPECT_EQ(GetProgramResourceLocation(p, GL_PROGRAM_OUTPUT, "gl_SampleMask"), -1);
        EXPECT_EQ(GetProgramResourceLocationIndex(p, GL_PROGRAM_OUTPUT, "gl_FragDepth"), -1);
        EXPECT_EQ(GetProgramResourceLocationIndex(p, GL_PROGRAM_OUTPUT, "gl_SampleMask[0]"), -1);

        const std::vector<GLenum> props = {GL_NAME_LENGTH, GL_TYPE, GL_ARRAY_SIZE, GL_REFERENCED_BY_FRAGMENT_SHADER,
                                           GL_LOCATION,    GL_LOCATION_INDEX};
        EXPECT_EQ(PropsOf(p, GL_PROGRAM_OUTPUT, "gl_FragDepth", props),
                  (std::vector<GLint>{13, GL_FLOAT, 1, 1, -1, -1}));
        EXPECT_EQ(PropsOf(p, GL_PROGRAM_OUTPUT, "gl_SampleMask[0]", props),
                  (std::vector<GLint>{17, GL_INT, 1, 1, -1, -1}));
        EXPECT_EQ(TakeError(), GL_NO_ERROR);
    }

    // ---------------------------------------------------------------- input-built-in ----
    TEST_F(ProgramInterfaceTest, InputBuiltInsUseGlSpellings) {
        const char* vs = R"(#version 430
void main(void) { gl_Position = (gl_VertexID + gl_InstanceID) * vec4(0.1); }
)";
        const GLuint p = MakeProgram(vs, kSimpleFs);
        BindFragDataLocation(p, 0, "color");
        LinkProgram(p);
        ExpectLinked(p);
        ClearErrors();

        EXPECT_EQ(Interfaceiv(p, GL_PROGRAM_INPUT, GL_ACTIVE_RESOURCES), 2);
        EXPECT_EQ(Interfaceiv(p, GL_PROGRAM_INPUT, GL_MAX_NAME_LENGTH), 14);
        // The Vulkan-semantics parse calls these gl_VertexIndex / gl_InstanceIndex.
        ExpectResource(p, GL_PROGRAM_INPUT, "gl_VertexID", "gl_VertexID");
        ExpectResource(p, GL_PROGRAM_INPUT, "gl_InstanceID", "gl_InstanceID");
        EXPECT_EQ(GetProgramResourceLocation(p, GL_PROGRAM_INPUT, "gl_VertexID"), -1);
        EXPECT_EQ(GetProgramResourceLocation(p, GL_PROGRAM_INPUT, "gl_InstanceID"), -1);

        const std::vector<GLenum> props = {GL_NAME_LENGTH, GL_TYPE, GL_ARRAY_SIZE, GL_REFERENCED_BY_VERTEX_SHADER,
                                           GL_LOCATION};
        EXPECT_EQ(PropsOf(p, GL_PROGRAM_INPUT, "gl_VertexID", props), (std::vector<GLint>{12, GL_INT, 1, 1, -1}));
        EXPECT_EQ(TakeError(), GL_NO_ERROR);
    }

    // ------------------------------------------------------------------ uniform-simple --
    TEST_F(ProgramInterfaceTest, UniformSimple) {
        const char* vs = R"(#version 430
in vec4 position;
uniform vec4 repos;
void main(void) { gl_Position = position + repos; }
)";
        const char* fs = R"(#version 430
uniform vec4 recolor;
out vec4 color;
void main() { color = vec4(0, 1, 0, 1) + recolor; }
)";
        const GLuint p = MakeProgram(vs, fs);
        BindAttribLocation(p, 0, "position");
        BindFragDataLocation(p, 0, "color");
        LinkProgram(p);
        ExpectLinked(p);
        ClearErrors();

        GLint activeUniforms = 0;
        GetProgramiv(p, GL_ACTIVE_UNIFORMS, &activeUniforms);
        EXPECT_EQ(Interfaceiv(p, GL_UNIFORM, GL_ACTIVE_RESOURCES), activeUniforms);
        EXPECT_EQ(Interfaceiv(p, GL_UNIFORM, GL_MAX_NAME_LENGTH), 8);
        ExpectResource(p, GL_UNIFORM, "repos", "repos");
        ExpectResource(p, GL_UNIFORM, "recolor", "recolor");

        // The sharpest single symptom of the old backend-forwarding design: these two had to
        // agree and did not, because the ESSL program keeps "repos" inside MGL_GLOBAL_UBO.
        EXPECT_EQ(GetProgramResourceLocation(p, GL_UNIFORM, "repos"), GetUniformLocation(p, "repos"));
        EXPECT_EQ(GetProgramResourceLocation(p, GL_UNIFORM, "recolor"), GetUniformLocation(p, "recolor"));

        const std::vector<GLenum> props = {GL_NAME_LENGTH,
                                           GL_TYPE,
                                           GL_ARRAY_SIZE,
                                           GL_OFFSET,
                                           GL_BLOCK_INDEX,
                                           GL_ARRAY_STRIDE,
                                           GL_MATRIX_STRIDE,
                                           GL_IS_ROW_MAJOR,
                                           GL_ATOMIC_COUNTER_BUFFER_INDEX,
                                           GL_REFERENCED_BY_COMPUTE_SHADER,
                                           GL_REFERENCED_BY_FRAGMENT_SHADER,
                                           GL_REFERENCED_BY_VERTEX_SHADER,
                                           GL_LOCATION};
        EXPECT_EQ(PropsOf(p, GL_UNIFORM, "repos", props),
                  (std::vector<GLint>{6, GL_FLOAT_VEC4, 1, -1, -1, -1, -1, 0, -1, 0, 0, 1,
                                      GetUniformLocation(p, "repos")}));
        EXPECT_EQ(PropsOf(p, GL_UNIFORM, "recolor", props),
                  (std::vector<GLint>{8, GL_FLOAT_VEC4, 1, -1, -1, -1, -1, 0, -1, 0, 1, 0,
                                      GetUniformLocation(p, "recolor")}));
        EXPECT_EQ(TakeError(), GL_NO_ERROR);
    }

    // ------------------------------------------------------------------- array-names ----
    TEST_F(ProgramInterfaceTest, ArrayNameSubscriptsAreStrictDecimals) {
        const char* vs = R"(#version 430
in vec4 position;
uniform vec4 a[2];
void main(void) { gl_Position = position + a[0] + a[1]; }
)";
        const GLuint p = MakeProgram(vs, kSimpleFs);
        BindAttribLocation(p, 0, "position");
        LinkProgram(p);
        ExpectLinked(p);
        ClearErrors();

        EXPECT_EQ(GetProgramResourceLocation(p, GL_UNIFORM, "a"), GetUniformLocation(p, "a"));
        EXPECT_EQ(GetProgramResourceLocation(p, GL_UNIFORM, "a[0]"), GetUniformLocation(p, "a"));
        EXPECT_EQ(GetProgramResourceLocation(p, GL_UNIFORM, "a[1]"), GetUniformLocation(p, "a[1]"));
        for (const char* bad : {"a[2]", "a[0 + 0]", "a[0+0]", "a[ 0]", "a[0 ]", "a[\n0]", "a[\t0]", "a[01]", "a[00]"}) {
            EXPECT_EQ(GetProgramResourceLocation(p, GL_UNIFORM, bad), -1) << "for '" << bad << "'";
        }
        EXPECT_EQ(TakeError(), GL_NO_ERROR);
    }

    // -------------------------------------------------------------- arrays-of-arrays ----
    TEST_F(ProgramInterfaceTest, ArraysOfArrays) {
        const char* vs = R"(#version 430
in vec4 position;
uniform vec4 a[3][4][5];
void main(void) {
    gl_Position = position;
    for (int i = 0; i < 5; ++i) gl_Position += a[2][1][i];
}
)";
        const GLuint p = MakeProgram(vs, kSimpleFs);
        LinkProgram(p);
        ExpectLinked(p);
        ClearErrors();

        EXPECT_EQ(Interfaceiv(p, GL_UNIFORM, GL_MAX_NAME_LENGTH), 11);
        ExpectResource(p, GL_UNIFORM, "a[2][1]", "a[2][1][0]");
        EXPECT_EQ(GetProgramResourceIndex(p, GL_UNIFORM, "a[2][1][0]"),
                  GetProgramResourceIndex(p, GL_UNIFORM, "a[2][1]"));

        const std::vector<GLenum> props = {GL_NAME_LENGTH,
                                           GL_TYPE,
                                           GL_ARRAY_SIZE,
                                           GL_OFFSET,
                                           GL_BLOCK_INDEX,
                                           GL_ARRAY_STRIDE,
                                           GL_MATRIX_STRIDE,
                                           GL_IS_ROW_MAJOR,
                                           GL_ATOMIC_COUNTER_BUFFER_INDEX,
                                           GL_REFERENCED_BY_VERTEX_SHADER,
                                           GL_LOCATION};
        EXPECT_EQ(PropsOf(p, GL_UNIFORM, "a[2][1]", props),
                  (std::vector<GLint>{11, GL_FLOAT_VEC4, 5, -1, -1, -1, -1, 0, -1, 1,
                                      GetUniformLocation(p, "a[2][1]")}));
        EXPECT_EQ(TakeError(), GL_NO_ERROR);
    }

    // ------------------------------------------------------------- uniform-block-types --
    TEST_F(ProgramInterfaceTest, UniformBlocks) {
        const char* vs = R"(#version 430
in vec4 position;
uniform SimpleBlock { mat3x2 a; mat4 b; vec4 c; };
uniform NotSoSimpleBlockk { ivec2 a[4]; mat3 b[2]; mat2 c; } d;
void main(void) {
    float tmp = a[0][1] * b[1][2] * c.x;
    tmp = tmp + d.a[2].y + d.b[0][1][1] + d.c[1][1];
    gl_Position = position * tmp;
}
)";
        const char* fs = R"(#version 430
struct U { bool a[3]; vec4 b; mat3 c; float d[2]; };
struct UU { U a; U b[2]; uvec2 c; };
uniform TrickyBlock { UU a[3]; mat4 b; uint c; } e[2];
out vec4 color;
void main() { color = vec4(0, 1, 0, 1) * e[0].a[2].b[0].d[1]; }
)";
        const GLuint p = MakeProgram(vs, fs);
        LinkProgram(p);
        ExpectLinked(p);
        ClearErrors();

        EXPECT_EQ(Interfaceiv(p, GL_UNIFORM_BLOCK, GL_ACTIVE_RESOURCES), 4);
        EXPECT_EQ(Interfaceiv(p, GL_UNIFORM_BLOCK, GL_MAX_NAME_LENGTH), 18);
        ExpectResource(p, GL_UNIFORM_BLOCK, "SimpleBlock", "SimpleBlock");
        ExpectResource(p, GL_UNIFORM_BLOCK, "TrickyBlock", "TrickyBlock[0]");
        ExpectResource(p, GL_UNIFORM_BLOCK, "TrickyBlock[1]", "TrickyBlock[1]");
        ExpectResource(p, GL_UNIFORM, "NotSoSimpleBlockk.a[0]", "NotSoSimpleBlockk.a[0]");
        ExpectResource(p, GL_UNIFORM, "TrickyBlock.a[2].b[0].d", "TrickyBlock.a[2].b[0].d[0]");

        const GLuint simple = GetProgramResourceIndex(p, GL_UNIFORM_BLOCK, "SimpleBlock");
        const GLuint tricky = GetProgramResourceIndex(p, GL_UNIFORM_BLOCK, "TrickyBlock");
        // The index the interface hands out must be usable with glUniformBlockBinding.
        UniformBlockBinding(p, simple, 0);
        UniformBlockBinding(p, tricky, 3);
        GLint dataSize = 0;
        GetActiveUniformBlockiv(p, simple, GL_UNIFORM_BLOCK_DATA_SIZE, &dataSize);
        EXPECT_EQ(Props(p, GL_UNIFORM_BLOCK, simple,
                        {GL_NAME_LENGTH, GL_BUFFER_BINDING, GL_REFERENCED_BY_FRAGMENT_SHADER,
                         GL_REFERENCED_BY_VERTEX_SHADER, GL_BUFFER_DATA_SIZE, GL_NUM_ACTIVE_VARIABLES}),
                  (std::vector<GLint>{12, 0, 0, 1, dataSize, 3}));
        EXPECT_EQ(Props(p, GL_UNIFORM_BLOCK, tricky,
                        {GL_NAME_LENGTH, GL_BUFFER_BINDING, GL_REFERENCED_BY_FRAGMENT_SHADER,
                         GL_REFERENCED_BY_VERTEX_SHADER}),
                  (std::vector<GLint>{15, 3, 1, 0}));

        // A block member reports its block, no location, and no atomic-counter buffer.
        EXPECT_EQ(PropsOf(p, GL_UNIFORM, "a",
                          {GL_NAME_LENGTH, GL_TYPE, GL_ARRAY_SIZE, GL_BLOCK_INDEX, GL_ARRAY_STRIDE, GL_IS_ROW_MAJOR,
                           GL_ATOMIC_COUNTER_BUFFER_INDEX, GL_REFERENCED_BY_VERTEX_SHADER, GL_LOCATION}),
                  (std::vector<GLint>{2, GL_FLOAT_MAT3x2, 1, static_cast<GLint>(simple), 0, 0, -1, 1, -1}));
        EXPECT_EQ(GetProgramResourceLocation(p, GL_UNIFORM, "a"), -1);
        EXPECT_EQ(GetProgramResourceLocation(p, GL_UNIFORM, "b"), -1);

        // GL_ACTIVE_VARIABLES lists exactly the three members, in GL_UNIFORM index space.
        const std::vector<GLint> activeVariables = Props(p, GL_UNIFORM_BLOCK, simple, {GL_ACTIVE_VARIABLES});
        ASSERT_EQ(activeVariables.size(), 3u);
        for (const GLint variable : activeVariables) {
            EXPECT_EQ(Props(p, GL_UNIFORM, static_cast<GLuint>(variable), {GL_BLOCK_INDEX}),
                      (std::vector<GLint>{static_cast<GLint>(simple)}));
        }
        EXPECT_GE(Interfaceiv(p, GL_UNIFORM_BLOCK, GL_MAX_NUM_ACTIVE_VARIABLES), 3);
        EXPECT_EQ(TakeError(), GL_NO_ERROR);
    }

    // ----------------------------------------------------------------- uniform-block-array
    TEST_F(ProgramInterfaceTest, UniformBlockArrayMemberReportsItsBlock) {
        const char* fs = R"(#version 430
uniform TestBlock { mediump vec4 color; } blockInstance[4];
out mediump vec4 color;
void main() { color = blockInstance[2].color + blockInstance[3].color; }
)";
        const GLuint p = MakeProgram(kSimpleVs, fs);
        LinkProgram(p);
        ExpectLinked(p);
        ClearErrors();

        const GLuint block = GetProgramResourceIndex(p, GL_UNIFORM_BLOCK, "TestBlock");
        ASSERT_NE(block, GL_INVALID_INDEX);
        EXPECT_EQ(PropsOf(p, GL_UNIFORM, "TestBlock.color", {GL_BLOCK_INDEX}),
                  (std::vector<GLint>{static_cast<GLint>(block)}));
        EXPECT_EQ(TakeError(), GL_NO_ERROR);
    }

    // ---------------------------------------------------------------------- ssb-types ----
    TEST_F(ProgramInterfaceTest, ShaderStorageBlocksAndBufferVariables) {
        const char* fs = R"(#version 430
struct U { bool a[3]; mediump vec4 b; mediump mat3 c; mediump float d[2]; };
struct UU { U a; U b[2]; uvec2 c; };
layout(binding=4) buffer TrickyBuffer { UU a[3]; mediump mat4 b; uint c; } e[2];
layout(binding = 0) buffer SimpleBuffer { mediump mat3x2 a; mediump mat4 b; mediump vec4 c; };
layout(binding = 1) buffer NotSoSimpleBuffer { ivec2 a[4]; mediump mat3 b[2]; mediump mat2 c; } d;
out mediump vec4 color;
void main() {
    mediump float tmp = e[0].a[0].b[0].d[0] * float(e[1].c);
    mediump float tmp2 = a[0][0] * b[0][0] * c.x;
    tmp2 = tmp2 + float(d.a[0].y) + d.b[0][0][0] + d.c[0][0];
    color = vec4(0, 1, 0, 1) * tmp * tmp2;
}
)";
        const GLuint p = MakeProgram(kSimpleVs, fs);
        LinkProgram(p);
        ExpectLinked(p);
        ClearErrors();

        EXPECT_EQ(Interfaceiv(p, GL_SHADER_STORAGE_BLOCK, GL_ACTIVE_RESOURCES), 4);
        EXPECT_EQ(Interfaceiv(p, GL_SHADER_STORAGE_BLOCK, GL_MAX_NAME_LENGTH), 18);
        EXPECT_EQ(Interfaceiv(p, GL_BUFFER_VARIABLE, GL_MAX_NAME_LENGTH), 28);
        EXPECT_GE(Interfaceiv(p, GL_BUFFER_VARIABLE, GL_ACTIVE_RESOURCES), 7);
        EXPECT_GE(Interfaceiv(p, GL_SHADER_STORAGE_BLOCK, GL_MAX_NUM_ACTIVE_VARIABLES), 3);

        ExpectResource(p, GL_SHADER_STORAGE_BLOCK, "SimpleBuffer", "SimpleBuffer");
        ExpectResource(p, GL_SHADER_STORAGE_BLOCK, "TrickyBuffer", "TrickyBuffer[0]");
        ExpectResource(p, GL_SHADER_STORAGE_BLOCK, "TrickyBuffer[1]", "TrickyBuffer[1]");
        // A member of a block with no instance name keeps its bare name.
        ExpectResource(p, GL_BUFFER_VARIABLE, "a", "a");
        ExpectResource(p, GL_BUFFER_VARIABLE, "NotSoSimpleBuffer.a[0]", "NotSoSimpleBuffer.a[0]");
        ExpectResource(p, GL_BUFFER_VARIABLE, "TrickyBuffer.a[0].b[0].d", "TrickyBuffer.a[0].b[0].d[0]");

        const GLuint simple = GetProgramResourceIndex(p, GL_SHADER_STORAGE_BLOCK, "SimpleBuffer");
        const GLuint tricky = GetProgramResourceIndex(p, GL_SHADER_STORAGE_BLOCK, "TrickyBuffer");
        const GLuint tricky1 = GetProgramResourceIndex(p, GL_SHADER_STORAGE_BLOCK, "TrickyBuffer[1]");
        EXPECT_EQ(Props(p, GL_SHADER_STORAGE_BLOCK, simple,
                        {GL_NAME_LENGTH, GL_BUFFER_BINDING, GL_NUM_ACTIVE_VARIABLES, GL_REFERENCED_BY_FRAGMENT_SHADER,
                         GL_REFERENCED_BY_VERTEX_SHADER}),
                  (std::vector<GLint>{13, 0, 3, 1, 0}));
        // An arrayed block gives element k the binding base + k.
        EXPECT_EQ(Props(p, GL_SHADER_STORAGE_BLOCK, tricky, {GL_NAME_LENGTH, GL_BUFFER_BINDING}),
                  (std::vector<GLint>{16, 4}));
        EXPECT_EQ(Props(p, GL_SHADER_STORAGE_BLOCK, tricky1, {GL_NAME_LENGTH, GL_BUFFER_BINDING}),
                  (std::vector<GLint>{16, 5}));

        EXPECT_EQ(PropsOf(p, GL_BUFFER_VARIABLE, "a",
                          {GL_NAME_LENGTH, GL_TYPE, GL_ARRAY_SIZE, GL_BLOCK_INDEX, GL_ARRAY_STRIDE, GL_IS_ROW_MAJOR,
                           GL_REFERENCED_BY_FRAGMENT_SHADER, GL_TOP_LEVEL_ARRAY_SIZE, GL_TOP_LEVEL_ARRAY_STRIDE}),
                  (std::vector<GLint>{2, GL_FLOAT_MAT3x2, 1, static_cast<GLint>(simple), 0, 0, 1, 1, 0}));
        EXPECT_EQ(PropsOf(p, GL_BUFFER_VARIABLE, "TrickyBuffer.a[0].b[0].d",
                          {GL_NAME_LENGTH, GL_TYPE, GL_ARRAY_SIZE, GL_BLOCK_INDEX, GL_MATRIX_STRIDE, GL_IS_ROW_MAJOR,
                           GL_REFERENCED_BY_FRAGMENT_SHADER, GL_TOP_LEVEL_ARRAY_SIZE}),
                  (std::vector<GLint>{28, GL_FLOAT, 2, static_cast<GLint>(tricky), 0, 0, 1, 3}));
        EXPECT_EQ(TakeError(), GL_NO_ERROR);
    }

    // --------------------------------------------------------------- top-level-array ----
    TEST_F(ProgramInterfaceTest, TopLevelArray) {
        const char* fs = R"(#version 430
buffer Block { vec4 a[5][4][3]; };
out vec4 color;
void main() { color = vec4(0, 1, 0, 1) + a[0][0][0]; }
)";
        const GLuint p = MakeProgram(kSimpleVs, fs);
        LinkProgram(p);
        ExpectLinked(p);
        ClearErrors();

        EXPECT_EQ(Interfaceiv(p, GL_BUFFER_VARIABLE, GL_MAX_NAME_LENGTH), 11);
        EXPECT_EQ(Interfaceiv(p, GL_SHADER_STORAGE_BLOCK, GL_MAX_NAME_LENGTH), 6);
        EXPECT_EQ(Interfaceiv(p, GL_SHADER_STORAGE_BLOCK, GL_ACTIVE_RESOURCES), 1);
        ExpectResource(p, GL_BUFFER_VARIABLE, "a[0][0]", "a[0][0][0]");
        ExpectResource(p, GL_SHADER_STORAGE_BLOCK, "Block", "Block");

        const GLuint block = GetProgramResourceIndex(p, GL_SHADER_STORAGE_BLOCK, "Block");
        EXPECT_EQ(PropsOf(p, GL_BUFFER_VARIABLE, "a[0][0]",
                          {GL_NAME_LENGTH, GL_TYPE, GL_ARRAY_SIZE, GL_BLOCK_INDEX, GL_IS_ROW_MAJOR,
                           GL_REFERENCED_BY_FRAGMENT_SHADER, GL_TOP_LEVEL_ARRAY_SIZE}),
                  (std::vector<GLint>{11, GL_FLOAT_VEC4, 3, static_cast<GLint>(block), 0, 1, 5}));
        const std::vector<GLint> stride = PropsOf(p, GL_BUFFER_VARIABLE, "a[0][0]", {GL_TOP_LEVEL_ARRAY_STRIDE});
        ASSERT_EQ(stride.size(), 1u);
        EXPECT_GT(stride[0], 0);
        EXPECT_EQ(TakeError(), GL_NO_ERROR);
    }

    // ---------------------------------------------------------------- compute-shader ----
    TEST_F(ProgramInterfaceTest, ComputeRuntimeSizedBufferVariable) {
        const char* cs = R"(#version 430 core
layout(local_size_x = 1, local_size_y = 1) in;
layout(std430) buffer Output { vec4 data[]; } g_out;
void main() {
   g_out.data[0] = vec4(1.0, 2.0, 3.0, 4.0);
   g_out.data[100] = vec4(1.0, 2.0, 3.0, 4.0);
}
)";
        const GLuint p = MakeProgram(nullptr, nullptr, cs);
        LinkProgram(p);
        ExpectLinked(p);
        ClearErrors();

        EXPECT_EQ(Interfaceiv(p, GL_BUFFER_VARIABLE, GL_MAX_NAME_LENGTH), 15);
        EXPECT_EQ(Interfaceiv(p, GL_BUFFER_VARIABLE, GL_ACTIVE_RESOURCES), 1);
        EXPECT_EQ(Interfaceiv(p, GL_SHADER_STORAGE_BLOCK, GL_ACTIVE_RESOURCES), 1);
        EXPECT_EQ(Interfaceiv(p, GL_SHADER_STORAGE_BLOCK, GL_MAX_NAME_LENGTH), 7);
        EXPECT_EQ(Interfaceiv(p, GL_SHADER_STORAGE_BLOCK, GL_MAX_NUM_ACTIVE_VARIABLES), 1);
        ExpectResource(p, GL_SHADER_STORAGE_BLOCK, "Output", "Output");
        ExpectResource(p, GL_BUFFER_VARIABLE, "Output.data", "Output.data[0]");

        const GLuint block = GetProgramResourceIndex(p, GL_SHADER_STORAGE_BLOCK, "Output");
        const GLuint variable = GetProgramResourceIndex(p, GL_BUFFER_VARIABLE, "Output.data");
        EXPECT_EQ(Props(p, GL_SHADER_STORAGE_BLOCK, block,
                        {GL_NAME_LENGTH, GL_BUFFER_BINDING, GL_NUM_ACTIVE_VARIABLES, GL_REFERENCED_BY_COMPUTE_SHADER,
                         GL_REFERENCED_BY_FRAGMENT_SHADER, GL_REFERENCED_BY_VERTEX_SHADER, GL_ACTIVE_VARIABLES}),
                  (std::vector<GLint>{7, 0, 1, 1, 0, 0, static_cast<GLint>(variable)}));
        // A runtime-sized array reports GL_ARRAY_SIZE 0 and a top-level array size of 1.
        EXPECT_EQ(Props(p, GL_BUFFER_VARIABLE, variable,
                        {GL_NAME_LENGTH, GL_TYPE, GL_ARRAY_SIZE, GL_BLOCK_INDEX, GL_IS_ROW_MAJOR,
                         GL_REFERENCED_BY_COMPUTE_SHADER, GL_TOP_LEVEL_ARRAY_SIZE}),
                  (std::vector<GLint>{15, GL_FLOAT_VEC4, 0, static_cast<GLint>(block), 0, 1, 1}));
        EXPECT_EQ(TakeError(), GL_NO_ERROR);
    }

    // --------------------------------------------------------------- atomic-counters ----
    TEST_F(ProgramInterfaceTest, AtomicCounterBuffers) {
        const char* fs = R"(#version 430
out vec4 color;
layout (binding = 1, offset = 0) uniform atomic_uint a;
layout (binding = 2, offset = 0) uniform atomic_uint b;
layout (binding = 2, offset = 4) uniform atomic_uint c;
layout (binding = 5, offset = 0) uniform atomic_uint d[3];
layout (binding = 5, offset = 12) uniform atomic_uint e;
void main() {
   uint x = atomicCounterIncrement(d[0]) + atomicCounterIncrement(a);
   uint y = atomicCounterIncrement(d[1]) + atomicCounterIncrement(b);
   uint z = atomicCounterIncrement(d[2]) + atomicCounterIncrement(c);
   uint w = atomicCounterIncrement(e);
   color = vec4(float(x), float(y), float(z), float(w));
}
)";
        const GLuint p = MakeProgram(kSimpleVs, fs);
        LinkProgram(p);
        ExpectLinked(p);
        ClearErrors();

        EXPECT_EQ(Interfaceiv(p, GL_ATOMIC_COUNTER_BUFFER, GL_ACTIVE_RESOURCES), 3);
        EXPECT_EQ(Interfaceiv(p, GL_ATOMIC_COUNTER_BUFFER, GL_MAX_NUM_ACTIVE_VARIABLES), 2);

        ExpectResource(p, GL_UNIFORM, "a", "a");
        ExpectResource(p, GL_UNIFORM, "d", "d[0]");
        for (const char* name : {"a", "b", "c", "d", "e", "d[0]", "d[1]", "d[2]"}) {
            EXPECT_EQ(GetProgramResourceLocation(p, GL_UNIFORM, name), -1) << name;
        }

        const auto bufferOf = [p](const char* uniform) {
            const std::vector<GLint> value = PropsOf(p, GL_UNIFORM, uniform, {GL_ATOMIC_COUNTER_BUFFER_INDEX});
            EXPECT_EQ(value.size(), 1u);
            return value.empty() ? -1 : value[0];
        };
        const GLint bufferA = bufferOf("a");
        const GLint bufferB = bufferOf("b");
        const GLint bufferD = bufferOf("d");
        ASSERT_GE(bufferA, 0);
        EXPECT_EQ(bufferB, bufferOf("c"));
        EXPECT_EQ(bufferD, bufferOf("e"));
        EXPECT_NE(bufferA, bufferB);

        EXPECT_EQ(Props(p, GL_ATOMIC_COUNTER_BUFFER, static_cast<GLuint>(bufferA),
                        {GL_BUFFER_BINDING, GL_BUFFER_DATA_SIZE, GL_NUM_ACTIVE_VARIABLES, GL_ACTIVE_VARIABLES}),
                  (std::vector<GLint>{1, 4, 1, static_cast<GLint>(GetProgramResourceIndex(p, GL_UNIFORM, "a"))}));
        EXPECT_EQ(Props(p, GL_ATOMIC_COUNTER_BUFFER, static_cast<GLuint>(bufferB),
                        {GL_BUFFER_BINDING, GL_BUFFER_DATA_SIZE, GL_NUM_ACTIVE_VARIABLES}),
                  (std::vector<GLint>{2, 8, 2}));
        EXPECT_EQ(Props(p, GL_ATOMIC_COUNTER_BUFFER, static_cast<GLuint>(bufferD),
                        {GL_BUFFER_BINDING, GL_BUFFER_DATA_SIZE, GL_NUM_ACTIVE_VARIABLES}),
                  (std::vector<GLint>{5, 16, 2}));
        EXPECT_EQ(TakeError(), GL_NO_ERROR);

        // The interface has no resource names at all.
        EXPECT_EQ(GetProgramResourceIndex(p, GL_ATOMIC_COUNTER_BUFFER, "a"), GL_INVALID_INDEX);
        EXPECT_EQ(TakeError(), GL_INVALID_ENUM);
    }

    // --------------------------------------------------------- transform-feedback ------
    TEST_F(ProgramInterfaceTest, TransformFeedbackVaryingTypes) {
        const char* vs = R"(#version 430
in vec4 position;
flat out ivec4 a;
out float b[2];
flat out uvec2 c;
flat out uint d;
out vec3 e[2];
flat out int f;
void main(void) {
   a = ivec4(1); b[0] = 1.1; b[1] = 1.1; c = uvec2(1u); d = 1u;
   e[0] = vec3(1.1); e[1] = vec3(1.1); f = 1;
   gl_Position = position;
}
)";
        const GLuint p = MakeProgram(vs, kSimpleFs);
        const char* varyings[6] = {"a", "b[0]", "b[1]", "c", "d", "e"};
        TransformFeedbackVaryings(p, 6, varyings, GL_INTERLEAVED_ATTRIBS);
        LinkProgram(p);
        ExpectLinked(p);
        ClearErrors();

        EXPECT_EQ(Interfaceiv(p, GL_TRANSFORM_FEEDBACK_VARYING, GL_ACTIVE_RESOURCES), 6);
        EXPECT_EQ(Interfaceiv(p, GL_TRANSFORM_FEEDBACK_VARYING, GL_MAX_NAME_LENGTH), 5);
        for (const char* name : varyings) ExpectResource(p, GL_TRANSFORM_FEEDBACK_VARYING, name, name);

        const std::vector<GLenum> props = {GL_NAME_LENGTH, GL_TYPE, GL_ARRAY_SIZE};
        EXPECT_EQ(PropsOf(p, GL_TRANSFORM_FEEDBACK_VARYING, "a", props),
                  (std::vector<GLint>{2, GL_INT_VEC4, 1}));
        EXPECT_EQ(PropsOf(p, GL_TRANSFORM_FEEDBACK_VARYING, "b[0]", props), (std::vector<GLint>{5, GL_FLOAT, 1}));
        EXPECT_EQ(PropsOf(p, GL_TRANSFORM_FEEDBACK_VARYING, "c", props),
                  (std::vector<GLint>{2, GL_UNSIGNED_INT_VEC2, 1}));
        EXPECT_EQ(PropsOf(p, GL_TRANSFORM_FEEDBACK_VARYING, "e", props),
                  (std::vector<GLint>{2, GL_FLOAT_VEC3, 2}));
        EXPECT_EQ(TakeError(), GL_NO_ERROR);
    }

    TEST_F(ProgramInterfaceTest, TransformFeedbackPseudoVaryingsAreEnumeratedButUnnamed) {
        const char* vs = R"(#version 430
in vec4 position;
out ivec4 a; out uvec2 c; out uint d; out int f; out uint e; out int g;
void main(void) {
   a = ivec4(1); c = uvec2(1u); d = 1u; f = 1; e = 1u; g = 1;
   gl_Position = position;
}
)";
        const GLuint p = MakeProgram(vs, kSimpleFs);
        const char* varyings[11] = {"a", "gl_NextBuffer",      "c", "gl_SkipComponents1", "d", "gl_SkipComponents2",
                                    "f", "gl_SkipComponents3", "e", "gl_SkipComponents4", "g"};
        TransformFeedbackVaryings(p, 11, varyings, GL_INTERLEAVED_ATTRIBS);
        LinkProgram(p);
        ExpectLinked(p);
        ClearErrors();

        // The capture list drops the layout controls; the interface must not.
        EXPECT_EQ(Interfaceiv(p, GL_TRANSFORM_FEEDBACK_VARYING, GL_ACTIVE_RESOURCES), 11);
        EXPECT_EQ(Interfaceiv(p, GL_TRANSFORM_FEEDBACK_VARYING, GL_MAX_NAME_LENGTH), 19);

        std::vector<std::string> names;
        for (GLuint i = 0; i < 11; ++i) names.push_back(ResourceName(p, GL_TRANSFORM_FEEDBACK_VARYING, i));
        const auto indexOf = [&names](const std::string& name) -> GLuint {
            for (GLuint i = 0; i < names.size(); ++i) {
                if (names[i] == name) return i;
            }
            return GL_INVALID_INDEX;
        };
        const std::vector<GLenum> props = {GL_NAME_LENGTH, GL_TYPE, GL_ARRAY_SIZE};
        ASSERT_NE(indexOf("gl_NextBuffer"), GL_INVALID_INDEX);
        EXPECT_EQ(Props(p, GL_TRANSFORM_FEEDBACK_VARYING, indexOf("gl_NextBuffer"), props),
                  (std::vector<GLint>{14, GL_NONE, 0}));
        EXPECT_EQ(Props(p, GL_TRANSFORM_FEEDBACK_VARYING, indexOf("gl_SkipComponents1"), props),
                  (std::vector<GLint>{19, GL_NONE, 1}));
        EXPECT_EQ(Props(p, GL_TRANSFORM_FEEDBACK_VARYING, indexOf("gl_SkipComponents4"), props),
                  (std::vector<GLint>{19, GL_NONE, 4}));
        // ...but they cannot be looked up by name.
        for (const char* name : {"gl_NextBuffer", "gl_SkipComponents1", "gl_SkipComponents4"}) {
            EXPECT_EQ(GetProgramResourceIndex(p, GL_TRANSFORM_FEEDBACK_VARYING, name), GL_INVALID_INDEX) << name;
        }
        EXPECT_EQ(TakeError(), GL_NO_ERROR);
    }

    // ------------------------------------------------------- separate-programs-fragment --
    TEST_F(ProgramInterfaceTest, SeparableFragmentProgramSeparatesUniformsFromBufferVariables) {
        const char* fs = R"(#version 430
out vec4 fs_color;
layout(location = 1) uniform vec4 x;
layout(binding = 0) buffer SimpleBuffer { vec4 a; };
in vec4 vs_color;
void main() { fs_color = vs_color + x + a; }
)";
        const GLuint p = CreateShaderProgramv(GL_FRAGMENT_SHADER, 1, &fs);
        ExpectLinked(p);
        ClearErrors();

        // KNOWN GAP, not an expectation - the same one SimpleShaders documents: a separable
        // FRAGMENT program's own inputs are absent from the glslang reflection, so
        // GL_PROGRAM_INPUT is empty. Spec-correct values here would be 1 and 9 ("vs_color").
        EXPECT_EQ(Interfaceiv(p, GL_PROGRAM_INPUT, GL_ACTIVE_RESOURCES), 0);
        EXPECT_EQ(Interfaceiv(p, GL_PROGRAM_INPUT, GL_MAX_NAME_LENGTH), 0);
        EXPECT_EQ(Interfaceiv(p, GL_PROGRAM_OUTPUT, GL_ACTIVE_RESOURCES), 1);
        EXPECT_EQ(Interfaceiv(p, GL_PROGRAM_OUTPUT, GL_MAX_NAME_LENGTH), 9);
        // The buffer variable is NOT a uniform, even though the frontend reflection keeps
        // both in one list.
        EXPECT_EQ(Interfaceiv(p, GL_UNIFORM, GL_ACTIVE_RESOURCES), 1);
        EXPECT_EQ(Interfaceiv(p, GL_UNIFORM, GL_MAX_NAME_LENGTH), 2);
        EXPECT_EQ(Interfaceiv(p, GL_BUFFER_VARIABLE, GL_ACTIVE_RESOURCES), 1);
        EXPECT_EQ(Interfaceiv(p, GL_BUFFER_VARIABLE, GL_MAX_NAME_LENGTH), 2);
        EXPECT_EQ(Interfaceiv(p, GL_SHADER_STORAGE_BLOCK, GL_ACTIVE_RESOURCES), 1);
        EXPECT_EQ(Interfaceiv(p, GL_SHADER_STORAGE_BLOCK, GL_MAX_NAME_LENGTH), 13);
        EXPECT_EQ(Interfaceiv(p, GL_SHADER_STORAGE_BLOCK, GL_MAX_NUM_ACTIVE_VARIABLES), 1);

        ExpectResource(p, GL_PROGRAM_OUTPUT, "fs_color", "fs_color");
        ExpectResource(p, GL_UNIFORM, "x", "x");
        ExpectResource(p, GL_SHADER_STORAGE_BLOCK, "SimpleBuffer", "SimpleBuffer");
        ExpectResource(p, GL_BUFFER_VARIABLE, "a", "a");
        EXPECT_EQ(GetProgramResourceLocation(p, GL_UNIFORM, "x"), 1);

        const GLuint block = GetProgramResourceIndex(p, GL_SHADER_STORAGE_BLOCK, "SimpleBuffer");
        const GLuint variable = GetProgramResourceIndex(p, GL_BUFFER_VARIABLE, "a");
        EXPECT_EQ(Props(p, GL_SHADER_STORAGE_BLOCK, block,
                        {GL_NAME_LENGTH, GL_BUFFER_BINDING, GL_NUM_ACTIVE_VARIABLES, GL_REFERENCED_BY_COMPUTE_SHADER,
                         GL_REFERENCED_BY_FRAGMENT_SHADER, GL_REFERENCED_BY_VERTEX_SHADER, GL_ACTIVE_VARIABLES}),
                  (std::vector<GLint>{13, 0, 1, 0, 1, 0, static_cast<GLint>(variable)}));
        EXPECT_EQ(Props(p, GL_BUFFER_VARIABLE, variable,
                        {GL_NAME_LENGTH, GL_TYPE, GL_ARRAY_SIZE, GL_BLOCK_INDEX, GL_ARRAY_STRIDE, GL_IS_ROW_MAJOR,
                         GL_REFERENCED_BY_FRAGMENT_SHADER, GL_TOP_LEVEL_ARRAY_SIZE, GL_TOP_LEVEL_ARRAY_STRIDE}),
                  (std::vector<GLint>{2, GL_FLOAT_VEC4, 1, static_cast<GLint>(block), 0, 0, 1, 1, 0}));
        EXPECT_EQ(TakeError(), GL_NO_ERROR);
    }

    // ---------------------------------------------------------------- error handling ----
    TEST_F(ProgramInterfaceTest, UnlinkedProgramHasZeroResourcesAndRaisesNoError) {
        const GLuint p = CreateProgram();
        ClearErrors();

        for (const GLenum iface : {GL_PROGRAM_INPUT, GL_PROGRAM_OUTPUT, GL_UNIFORM, GL_UNIFORM_BLOCK,
                                   GL_BUFFER_VARIABLE, GL_SHADER_STORAGE_BLOCK, GL_TRANSFORM_FEEDBACK_VARYING,
                                   GL_VERTEX_SUBROUTINE, GL_FRAGMENT_SUBROUTINE_UNIFORM}) {
            EXPECT_EQ(Interfaceiv(p, iface, GL_ACTIVE_RESOURCES), 0) << std::hex << iface;
            EXPECT_EQ(Interfaceiv(p, iface, GL_MAX_NAME_LENGTH), 0) << std::hex << iface;
            EXPECT_EQ(GetProgramResourceIndex(p, iface, ""), GL_INVALID_INDEX) << std::hex << iface;
        }
        EXPECT_EQ(Interfaceiv(p, GL_ATOMIC_COUNTER_BUFFER, GL_ACTIVE_RESOURCES), 0);
        EXPECT_EQ(Interfaceiv(p, GL_ATOMIC_COUNTER_BUFFER, GL_MAX_NUM_ACTIVE_VARIABLES), 0);
        EXPECT_EQ(Interfaceiv(p, GL_UNIFORM_BLOCK, GL_MAX_NUM_ACTIVE_VARIABLES), 0);
        // Not one stray error - a leftover here aborts the caller's next query.
        EXPECT_EQ(TakeError(), GL_NO_ERROR);

        // Locations, however, really do require a successful link.
        EXPECT_EQ(GetProgramResourceLocation(p, GL_PROGRAM_INPUT, "pie"), -1);
        EXPECT_EQ(TakeError(), GL_INVALID_OPERATION);
        EXPECT_EQ(GetProgramResourceLocationIndex(p, GL_PROGRAM_OUTPUT, "pie"), -1);
        EXPECT_EQ(TakeError(), GL_INVALID_OPERATION);
    }

    TEST_F(ProgramInterfaceTest, ErrorConditions) {
        const GLuint p = MakeProgram(kSimpleVs, kSimpleFs);
        BindAttribLocation(p, 0, "position");
        BindFragDataLocation(p, 0, "color");
        LinkProgram(p);
        ExpectLinked(p);
        ClearErrors();

        GLint value = 0;
        GLsizei length = 0;
        GLchar name[100] = {'\0'};

        // <program> is not a name at all.
        GetProgramInterfaceiv(1337u, GL_PROGRAM_INPUT, GL_ACTIVE_RESOURCES, &value);
        EXPECT_EQ(TakeError(), GL_INVALID_VALUE);
        GetProgramResourceIndex(1337u, GL_PROGRAM_INPUT, "pie");
        EXPECT_EQ(TakeError(), GL_INVALID_VALUE);
        GetProgramResourceLocation(1337u, GL_PROGRAM_INPUT, "pie");
        EXPECT_EQ(TakeError(), GL_INVALID_VALUE);

        // <program> names a shader object.
        const GLuint shader = CreateShader(GL_FRAGMENT_SHADER);
        GetProgramInterfaceiv(shader, GL_PROGRAM_INPUT, GL_ACTIVE_RESOURCES, &value);
        EXPECT_EQ(TakeError(), GL_INVALID_OPERATION);
        GetProgramResourceIndex(shader, GL_PROGRAM_INPUT, "pie");
        EXPECT_EQ(TakeError(), GL_INVALID_OPERATION);

        // <index> past the end.
        GetProgramResourceName(p, GL_PROGRAM_INPUT, 3000, 1024, &length, name);
        EXPECT_EQ(TakeError(), GL_INVALID_VALUE);
        // propCount == 0.
        GLenum props[1] = {GL_NAME_LENGTH};
        GetProgramResourceiv(p, GL_PROGRAM_INPUT, 0, 0, props, 1024, &length, &value);
        EXPECT_EQ(TakeError(), GL_INVALID_VALUE);
        // Negative sizes.
        GetProgramResourceName(p, GL_PROGRAM_INPUT, 0, -100, nullptr, name);
        EXPECT_EQ(TakeError(), GL_INVALID_VALUE);
        GetProgramResourceiv(p, GL_PROGRAM_INPUT, 0, 1, props, -100, &length, &value);
        EXPECT_EQ(TakeError(), GL_INVALID_VALUE);

        // A prop this command does not know at all vs. one the interface does not carry.
        GLenum unknownProp[1] = {GL_TEXTURE_1D};
        GetProgramResourceiv(p, GL_PROGRAM_INPUT, 0, 1, unknownProp, 1024, &length, &value);
        EXPECT_EQ(TakeError(), GL_INVALID_ENUM);
        GLenum wrongInterfaceProp[1] = {GL_OFFSET};
        GetProgramResourceiv(p, GL_PROGRAM_INPUT, 0, 1, wrongInterfaceProp, 1024, &length, &value);
        EXPECT_EQ(TakeError(), GL_INVALID_OPERATION);

        // GL_ATOMIC_COUNTER_BUFFER has no names, and no locations.
        GetProgramResourceName(p, GL_ATOMIC_COUNTER_BUFFER, 0, 1024, &length, name);
        EXPECT_EQ(TakeError(), GL_INVALID_ENUM);
        GetProgramResourceLocation(p, GL_ATOMIC_COUNTER_BUFFER, "position");
        EXPECT_EQ(TakeError(), GL_INVALID_ENUM);
        // ...and GetProgramInterfaceiv rejects a pname the interface does not answer.
        GetProgramInterfaceiv(p, GL_PROGRAM_INPUT, GL_MAX_NUM_ACTIVE_VARIABLES, &value);
        EXPECT_EQ(TakeError(), GL_INVALID_OPERATION);
    }

    TEST_F(ProgramInterfaceTest, BufSizeIsRespected) {
        const char* vs = R"(#version 430
in vec4 position;
uniform vec4 someLongName;
void main(void) { gl_Position = position + someLongName; }
)";
        const GLuint p = MakeProgram(vs, kSimpleFs);
        LinkProgram(p);
        ExpectLinked(p);
        ClearErrors();

        const GLuint index = GetProgramResourceIndex(p, GL_UNIFORM, "someLongName");
        ASSERT_NE(index, GL_INVALID_INDEX);
        GLchar buffer[3] = {'a', 'b', 'c'};
        GLsizei length = -1;
        GetProgramResourceName(p, GL_UNIFORM, index, 0, nullptr, nullptr);
        GetProgramResourceName(p, GL_UNIFORM, index, 0, nullptr, buffer);
        EXPECT_EQ(buffer[0], 'a');
        EXPECT_EQ(buffer[2], 'c');
        GetProgramResourceName(p, GL_UNIFORM, index, 2, &length, buffer);
        EXPECT_EQ(buffer[0], 's');
        EXPECT_EQ(buffer[1], '\0');
        EXPECT_EQ(buffer[2], 'c');
        EXPECT_EQ(length, 1);

        GLint params[3] = {1, 2, 3};
        const GLenum props[] = {GL_NAME_LENGTH, GL_TYPE, GL_ARRAY_SIZE, GL_OFFSET, GL_BLOCK_INDEX, GL_LOCATION};
        GetProgramResourceiv(p, GL_UNIFORM, index, 6, props, 0, nullptr, nullptr);
        GetProgramResourceiv(p, GL_UNIFORM, index, 6, props, 0, nullptr, params);
        EXPECT_EQ(params[0], 1);
        EXPECT_EQ(params[2], 3);
        GetProgramResourceiv(p, GL_UNIFORM, index, 6, props, 2, &length, params);
        EXPECT_EQ(params[0], 13);
        EXPECT_EQ(params[1], GL_FLOAT_VEC4);
        EXPECT_EQ(params[2], 3);
        EXPECT_EQ(length, 2);
        EXPECT_EQ(TakeError(), GL_NO_ERROR);
    }

    // ------------------------------------------------------------- buffer binding state ----
    // GL 4.6 §7.3.1 makes ONE index space out of glGetProgramResourceIndex and the command
    // that consumes its answer: the index glGetProgramResourceIndex(GL_SHADER_STORAGE_BLOCK)
    // returns IS the index glShaderStorageBlockBinding takes. The declared bindings below are
    // deliberately NOT the enumeration order, so a binding applied through a different index
    // space lands on the wrong block instead of failing loudly.
    const char* kStorageBlockFs = R"(#version 430
layout(binding = 3) buffer BlockA { vec4 a; };
layout(binding = 1) buffer BlockB { vec4 b; };
layout(binding = 2) buffer BlockC { vec4 c; };
out vec4 color;
void main() { color = a + b + c; }
)";

    // A binding read straight back through GL_BUFFER_BINDING, by NAME, so the assertion does
    // not depend on the enumeration order it is meant to be checking.
    GLint BufferBindingOf(GLuint program, GLenum iface, const char* name) {
        const std::vector<GLint> values = PropsOf(program, iface, name, {GL_BUFFER_BINDING});
        return values.size() == 1 ? values[0] : -12345;
    }

    TEST_F(ProgramInterfaceTest, ShaderStorageBlockBindingTakesTheResourceQueryIndex) {
        const GLuint p = MakeProgram(kSimpleVs, kStorageBlockFs);
        LinkProgram(p);
        ExpectLinked(p);
        ClearErrors();

        // Bound first, the way a real caller reaches glShaderStorageBlockBinding. It matters
        // because that entry point also delegates to the backend, and asking a BACKEND to
        // build a program for the first time from inside a non-draw entry point is a
        // pre-existing DirectGLES hazard (GetBackendProgramId -> SyncToBackend runs without
        // the draw-path globals SyncCurrentProgram would have established; under a loaded
        // llvmpipe it throws out of the transpile). Nothing about the index space under test
        // depends on this - it just keeps the case testing the frontend contract.
        UseProgram(p);
        ASSERT_EQ(Interfaceiv(p, GL_SHADER_STORAGE_BLOCK, GL_ACTIVE_RESOURCES), 3);
        // Until something rebinds them, GL_BUFFER_BINDING is what the shader declared.
        EXPECT_EQ(BufferBindingOf(p, GL_SHADER_STORAGE_BLOCK, "BlockA"), 3);
        EXPECT_EQ(BufferBindingOf(p, GL_SHADER_STORAGE_BLOCK, "BlockB"), 1);
        EXPECT_EQ(BufferBindingOf(p, GL_SHADER_STORAGE_BLOCK, "BlockC"), 2);
        EXPECT_EQ(TakeError(), GL_NO_ERROR);

        // THE ROUND TRIP. BlockB is the interesting one: its enumeration index (1) and its
        // declared binding (1) coincide, while BlockA's do not, so an implementation that
        // confused index with binding would still pass on B alone - hence all three are
        // re-read afterwards.
        const GLuint blockA = GetProgramResourceIndex(p, GL_SHADER_STORAGE_BLOCK, "BlockA");
        ASSERT_NE(blockA, GL_INVALID_INDEX);
        ShaderStorageBlockBinding(p, blockA, 6);
        // GPU-free suite: with no backend bound the call still records the binding on the
        // program (that is the state GL_BUFFER_BINDING reports) and then reports that it
        // could not reach a driver. Swallow exactly that.
        ClearErrors();

        EXPECT_EQ(BufferBindingOf(p, GL_SHADER_STORAGE_BLOCK, "BlockA"), 6) << "the rebound block";
        EXPECT_EQ(BufferBindingOf(p, GL_SHADER_STORAGE_BLOCK, "BlockB"), 1) << "must not move";
        EXPECT_EQ(BufferBindingOf(p, GL_SHADER_STORAGE_BLOCK, "BlockC"), 2) << "must not move";
        EXPECT_EQ(TakeError(), GL_NO_ERROR);

        // And it survives a second, different rebinding of another block.
        const GLuint blockC = GetProgramResourceIndex(p, GL_SHADER_STORAGE_BLOCK, "BlockC");
        ASSERT_NE(blockC, GL_INVALID_INDEX);
        ShaderStorageBlockBinding(p, blockC, 0);
        ClearErrors();
        EXPECT_EQ(BufferBindingOf(p, GL_SHADER_STORAGE_BLOCK, "BlockA"), 6);
        EXPECT_EQ(BufferBindingOf(p, GL_SHADER_STORAGE_BLOCK, "BlockB"), 1);
        EXPECT_EQ(BufferBindingOf(p, GL_SHADER_STORAGE_BLOCK, "BlockC"), 0);
        EXPECT_EQ(TakeError(), GL_NO_ERROR);
    }

    TEST_F(ProgramInterfaceTest, ShaderStorageBlockBindingRejectsAnIndexOutsideTheInterface) {
        const GLuint p = MakeProgram(kSimpleVs, kStorageBlockFs);
        LinkProgram(p);
        ExpectLinked(p);
        ClearErrors();

        const GLint activeBlocks = Interfaceiv(p, GL_SHADER_STORAGE_BLOCK, GL_ACTIVE_RESOURCES);
        ASSERT_EQ(activeBlocks, 3);
        ShaderStorageBlockBinding(p, static_cast<GLuint>(activeBlocks), 4);
        EXPECT_EQ(TakeError(), GL_INVALID_VALUE);
        ClearErrors();

        // Nothing moved.
        EXPECT_EQ(BufferBindingOf(p, GL_SHADER_STORAGE_BLOCK, "BlockA"), 3);
        EXPECT_EQ(BufferBindingOf(p, GL_SHADER_STORAGE_BLOCK, "BlockB"), 1);
        EXPECT_EQ(BufferBindingOf(p, GL_SHADER_STORAGE_BLOCK, "BlockC"), 2);
        EXPECT_EQ(TakeError(), GL_NO_ERROR);
    }

    // The same rule on the uniform side: GL_BUFFER_BINDING is the CURRENT binding, and
    // GL_UNIFORM_BLOCK's index space is the one glGetUniformBlockIndex / glUniformBlockBinding
    // already use.
    TEST_F(ProgramInterfaceTest, UniformBlockBufferBindingFollowsUniformBlockBinding) {
        const char* fs = R"(#version 430
layout(binding = 2) uniform BlockU { vec4 u; };
layout(binding = 0) uniform BlockV { vec4 v; };
out vec4 color;
void main() { color = u + v; }
)";
        const GLuint p = MakeProgram(kSimpleVs, fs);
        LinkProgram(p);
        ExpectLinked(p);
        ClearErrors();

        ASSERT_EQ(Interfaceiv(p, GL_UNIFORM_BLOCK, GL_ACTIVE_RESOURCES), 2);
        EXPECT_EQ(BufferBindingOf(p, GL_UNIFORM_BLOCK, "BlockU"), 2);
        EXPECT_EQ(BufferBindingOf(p, GL_UNIFORM_BLOCK, "BlockV"), 0);

        // One index space, both directions.
        const GLuint interfaceIndex = GetProgramResourceIndex(p, GL_UNIFORM_BLOCK, "BlockU");
        ASSERT_NE(interfaceIndex, GL_INVALID_INDEX);
        EXPECT_EQ(interfaceIndex, GetUniformBlockIndex(p, "BlockU"));
        EXPECT_EQ(TakeError(), GL_NO_ERROR);

        UniformBlockBinding(p, interfaceIndex, 5);
        ClearErrors();
        EXPECT_EQ(BufferBindingOf(p, GL_UNIFORM_BLOCK, "BlockU"), 5) << "the rebound block";
        EXPECT_EQ(BufferBindingOf(p, GL_UNIFORM_BLOCK, "BlockV"), 0) << "must not move";
        // glGetActiveUniformBlockiv is the older spelling of the same state; the two must not
        // be able to disagree.
        GLint viaActiveUniformBlockiv = -1;
        GetActiveUniformBlockiv(p, interfaceIndex, GL_UNIFORM_BLOCK_BINDING, &viaActiveUniformBlockiv);
        EXPECT_EQ(viaActiveUniformBlockiv, 5);
        EXPECT_EQ(TakeError(), GL_NO_ERROR);
    }
} // namespace
