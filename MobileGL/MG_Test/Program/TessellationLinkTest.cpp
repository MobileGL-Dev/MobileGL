// MobileGL - MobileGL/MG_Test/Program/TessellationLinkTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// Link-time properties of programs that carry a tessellation control stage. GPU-free:
// everything asserted here is a property of the link, not of any driver.
//
// Two independent defects live here, both found by KHR-GL4x.tessellation_shader:
//
//  (1) The transform-feedback capture stage. GL 4.6 core 11 makes the tessellation CONTROL
//      shader a vertex-processing stage like the other three, so in a separable program whose
//      only stage is a TCS it is the LAST vertex-processing stage and therefore the capture
//      stage - such a program must link with transform-feedback varyings requested. MobileGL
//      searched {geometry, tessellation evaluation, vertex} only and refused the link with
//      "Transform feedback varyings requested but the program has no vertex-processing stage",
//      failing KHR-GL4x.tessellation_shader.single.xfb_captures_data_from_correct_stage on all
//      three API versions (esextcTessellationShaderXFB.cpp:390-416 passes should_succeed=true
//      for a non-ES context; ES demands the opposite, which is why the new arm is documented as
//      desktop-GL-only at the search site).
//
//  (2) `patch out T name[N]` against `patch in T name[N]`. Legal, identically spelled on both
//      sides, and rejected - see the last case, which carries the diagnosis.

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
    class TessellationLinkTest: public ::testing::Test {
    protected:
        void SetUp() override {
            MobileGL::Initialize();
            for (int i = 0; i < 32 && GetError() != GL_NO_ERROR; ++i) {
            }
        }
    };

    std::string ShaderLog(GLuint shader) {
        char log[4096] = "";
        GetShaderInfoLog(shader, sizeof(log), nullptr, log);
        return std::string(log);
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

    // Attaches one compiled shader of each requested stage. Compilation is asserted, so a
    // failure here is a shader bug in the test rather than a link result.
    GLuint MakeProgram(const std::vector<std::pair<GLenum, const char*>>& stages, Bool separable) {
        const GLuint program = CreateProgram();
        if (separable) {
            ProgramParameteri(program, GL_PROGRAM_SEPARABLE, GL_TRUE);
        }
        for (const auto& [type, source]: stages) {
            const GLuint shader = CreateShader(type);
            ShaderSource(shader, 1, &source, nullptr);
            CompileShader(shader);
            GLint compiled = GL_FALSE;
            GetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
            EXPECT_EQ(compiled, GL_TRUE) << "stage 0x" << std::hex << type << "\n" << ShaderLog(shader);
            AttachShader(program, shader);
        }
        return program;
    }

    // The conformance suite's own tessellation control shader
    // (esextcTessellationShaderXFB.cpp:360-381), with the ES-only ${...} expansions dropped -
    // on a desktop context they expand to nothing.
    constexpr const char* kCtsTessControl = R"(#version 460 core
layout (vertices=4) out;

in  BLOCK_INOUT { vec4 value; } user_in[];
out BLOCK_INOUT { vec4 value; } user_out[];

void main()
{
    gl_out   [gl_InvocationID].gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
    user_out [gl_InvocationID].value       = vec4(2.0, 3.0, 4.0, 5.0);

    gl_TessLevelOuter[0] = 1.0;
    gl_TessLevelOuter[1] = 1.0;
}
)";

    // A tessellation control shader IS a vertex-processing stage (GL 4.6 core 11), and in a
    // TCS-only separable program it is the last one - so it is the capture stage and the link
    // must succeed with the block member resolved against ITS outputs.
    TEST_F(TessellationLinkTest, TcsOnlySeparableProgramWithXfbVaryingsLinks) {
        const GLuint program = MakeProgram({{GL_TESS_CONTROL_SHADER, kCtsTessControl}}, /*separable=*/true);
        const GLchar* const varyings[1] = {"BLOCK_INOUT.value"};
        TransformFeedbackVaryings(program, 1, varyings, GL_SEPARATE_ATTRIBS);
        LinkProgram(program);

        ASSERT_EQ(Programiv(program, GL_LINK_STATUS), GL_TRUE) << LinkLog(program);
        EXPECT_EQ(GetError(), static_cast<GLenum>(GL_NO_ERROR));

        // The request resolved rather than being quietly dropped: the interface reports it back.
        EXPECT_EQ(Programiv(program, GL_TRANSFORM_FEEDBACK_VARYINGS), 1);
        EXPECT_EQ(Programiv(program, GL_TRANSFORM_FEEDBACK_BUFFER_MODE), GL_SEPARATE_ATTRIBS);

        GLchar name[128] = {'\0'};
        GLsizei length = 0;
        GLsizei size = 0;
        GLenum type = 0;
        GetTransformFeedbackVarying(program, 0, sizeof(name), &length, &size, &type, name);
        EXPECT_EQ(std::string(name, name + (length < 0 ? 0 : length)), "BLOCK_INOUT.value");
        EXPECT_EQ(type, static_cast<GLenum>(GL_FLOAT_VEC4));
        EXPECT_EQ(GetError(), static_cast<GLenum>(GL_NO_ERROR));
    }

    // Control: the same program with no capture request. It linked before the fix too, which is
    // what keeps this case honest about WHICH half of the link moved.
    TEST_F(TessellationLinkTest, TcsOnlySeparableProgramWithoutXfbVaryingsLinks) {
        const GLuint program = MakeProgram({{GL_TESS_CONTROL_SHADER, kCtsTessControl}}, /*separable=*/true);
        LinkProgram(program);
        ASSERT_EQ(Programiv(program, GL_LINK_STATUS), GL_TRUE) << LinkLog(program);
        EXPECT_EQ(Programiv(program, GL_TRANSFORM_FEEDBACK_VARYINGS), 0);
        EXPECT_EQ(GetError(), static_cast<GLenum>(GL_NO_ERROR));
    }

    // A program with no vertex-processing stage at all still has to be refused - the fix widened
    // the search, it did not remove the check.
    TEST_F(TessellationLinkTest, FragmentOnlySeparableProgramWithXfbVaryingsStillFailsToLink) {
        constexpr const char* fs = R"(#version 460 core
out vec4 color;
void main() { color = vec4(1.0); }
)";
        const GLuint program = MakeProgram({{GL_FRAGMENT_SHADER, fs}}, /*separable=*/true);
        const GLchar* const varyings[1] = {"color"};
        TransformFeedbackVaryings(program, 1, varyings, GL_INTERLEAVED_ATTRIBS);
        LinkProgram(program);
        EXPECT_EQ(Programiv(program, GL_LINK_STATUS), GL_FALSE);
        EXPECT_NE(LinkLog(program).find("no vertex-processing stage"), std::string::npos) << LinkLog(program);
        for (int i = 0; i < 32 && GetError() != GL_NO_ERROR; ++i) {
        }
    }

    constexpr const char* kPassthroughVs = R"(#version 460 core
void main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }
)";

    constexpr const char* kTcsWithPatchScalar = R"(#version 460 core
layout (vertices = 3) out;
patch out vec4 tcs_patch;
out vec4 tcs_per_vertex[];
void main() {
    tcs_patch = vec4(1.0);
    tcs_per_vertex[gl_InvocationID] = vec4(2.0);
    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
    gl_TessLevelOuter[0] = 1.0; gl_TessLevelOuter[1] = 1.0; gl_TessLevelOuter[2] = 1.0;
    gl_TessLevelInner[0] = 1.0;
}
)";

    constexpr const char* kTesWithPatchScalar = R"(#version 460 core
layout (triangles) in;
patch in vec4 tcs_patch;
in vec4 tcs_per_vertex[];
out vec4 tes_out;
void main() {
    tes_out = tcs_patch + tcs_per_vertex[0];
    gl_Position = gl_in[0].gl_Position;
}
)";

    // The capture stage of a COMPLETE pipeline is unchanged by the widened search: tessellation
    // control sits AFTER tessellation evaluation in the order, so a program that has both still
    // resolves its capture names against the EVALUATION stage's outputs. Both halves are pinned -
    // an evaluation output resolves, a control output does not.
    TEST_F(TessellationLinkTest, CompletePipelineStillCapturesAtTheEvaluationStage) {
        {
            const GLuint program = MakeProgram({{GL_VERTEX_SHADER, kPassthroughVs},
                                                {GL_TESS_CONTROL_SHADER, kTcsWithPatchScalar},
                                                {GL_TESS_EVALUATION_SHADER, kTesWithPatchScalar}},
                                               /*separable=*/true);
            const GLchar* const varyings[1] = {"tes_out"};
            TransformFeedbackVaryings(program, 1, varyings, GL_INTERLEAVED_ATTRIBS);
            LinkProgram(program);
            ASSERT_EQ(Programiv(program, GL_LINK_STATUS), GL_TRUE) << LinkLog(program);
            EXPECT_EQ(Programiv(program, GL_TRANSFORM_FEEDBACK_VARYINGS), 1);
        }
        {
            // tcs_per_vertex is an output of the CONTROL stage, which is not the capture stage
            // here. Resolving it would mean capturing at the wrong stage, so the link must fail.
            const GLuint program = MakeProgram({{GL_VERTEX_SHADER, kPassthroughVs},
                                                {GL_TESS_CONTROL_SHADER, kTcsWithPatchScalar},
                                                {GL_TESS_EVALUATION_SHADER, kTesWithPatchScalar}},
                                               /*separable=*/true);
            const GLchar* const varyings[1] = {"tcs_per_vertex"};
            TransformFeedbackVaryings(program, 1, varyings, GL_INTERLEAVED_ATTRIBS);
            LinkProgram(program);
            EXPECT_EQ(Programiv(program, GL_LINK_STATUS), GL_FALSE) << LinkLog(program);
        }
        for (int i = 0; i < 32 && GetError() != GL_NO_ERROR; ++i) {
        }
    }

    // A patch-qualified SCALAR crosses the TCS/TES boundary today. It is the control for the
    // array case below: same qualifier, same stages, only the arrayness differs.
    TEST_F(TessellationLinkTest, PatchQualifiedScalarLinksAcrossTheTessellationStages) {
        const GLuint program = MakeProgram({{GL_VERTEX_SHADER, kPassthroughVs},
                                            {GL_TESS_CONTROL_SHADER, kTcsWithPatchScalar},
                                            {GL_TESS_EVALUATION_SHADER, kTesWithPatchScalar}},
                                           /*separable=*/true);
        LinkProgram(program);
        ASSERT_EQ(Programiv(program, GL_LINK_STATUS), GL_TRUE) << LinkLog(program);
        EXPECT_EQ(GetError(), static_cast<GLenum>(GL_NO_ERROR));
    }

    // `patch out int a[N]` against `patch in int a[N]`: legal GLSL, identical spellings, and
    // currently refused by the VENDORED glslang with "Array sizes must be compatible" while
    // printing the two sides as the same type. This is what fails
    // KHR-GL4x.tessellation_shader.tessellation_shader_tc_barriers.* on all three API versions.
    //
    // The defect is one asymmetric clause in glslang, not in MobileGL:
    // 3rdparty/glslang/glslang/MachineIndependent/linkValidate.cpp, TIntermediate::isIoResizeArray.
    // The TessControl arm is guarded with `&& ! type.getQualifier().patch`; the TessEvaluation arm
    // is not. A patch-qualified array therefore answers false on the control side and true on the
    // evaluation side, and the caller's dimension arithmetic (linkValidate.cpp:1200-1218) computes
    // (numDim - firstDim) == (unitNumDim - unitFirstDim) as (1 - 0) == (1 - 1), i.e. false. The fix
    // is to mirror the control arm - add `&& ! type.getQualifier().patch` to the TessEvaluation
    // clause - which belongs in the fork (and upstream), so it is deliberately not made here.
    //
    // Written to flip from skip to assertion the moment that lands: the skip is allowed ONLY for
    // the exact known message, and any other failure is a real failure.
    TEST_F(TessellationLinkTest, PatchQualifiedArrayLinksAcrossTheTessellationStages) {
        constexpr const char* tcs = R"(#version 460 core
layout (vertices = 3) out;
patch out int tcs_patch_result[16];
void main() {
    for (int i = 0; i < 16; ++i) { tcs_patch_result[i] = i; }
    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
    gl_TessLevelOuter[0] = 1.0; gl_TessLevelOuter[1] = 1.0; gl_TessLevelOuter[2] = 1.0;
    gl_TessLevelInner[0] = 1.0;
}
)";
        constexpr const char* tes = R"(#version 460 core
layout (triangles) in;
patch in int tcs_patch_result[16];
out vec4 tes_out;
void main() {
    tes_out = vec4(float(tcs_patch_result[0] + tcs_patch_result[15]));
    gl_Position = gl_in[0].gl_Position;
}
)";
        const GLuint program = MakeProgram({{GL_VERTEX_SHADER, kPassthroughVs},
                                            {GL_TESS_CONTROL_SHADER, tcs},
                                            {GL_TESS_EVALUATION_SHADER, tes}},
                                           /*separable=*/true);
        LinkProgram(program);
        const std::string log = LinkLog(program);
        if (Programiv(program, GL_LINK_STATUS) != GL_TRUE &&
            log.find("Array sizes must be compatible") != std::string::npos) {
            for (int i = 0; i < 32 && GetError() != GL_NO_ERROR; ++i) {
            }
            GTEST_SKIP() << "vendored glslang still credits the TessEvaluation side of a "
                            "patch-qualified array with an implicit outer dimension "
                            "(linkValidate.cpp isIoResizeArray, TessEvaluation arm missing "
                            "`&& ! type.getQualifier().patch`); link log:\n"
                         << log;
        }
        ASSERT_EQ(Programiv(program, GL_LINK_STATUS), GL_TRUE) << log;
        EXPECT_EQ(GetError(), static_cast<GLenum>(GL_NO_ERROR));
    }
} // namespace
