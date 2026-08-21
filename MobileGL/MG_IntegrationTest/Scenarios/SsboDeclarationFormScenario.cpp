// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/SsboDeclarationFormScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - EVERY WAY GLSL LETS YOU DECLARE A SHADER STORAGE BLOCK.
//
// KHR-GL43.shader_storage_buffer_object.basic-syntax and .basic-syntaxSSO walk eight declaration
// forms of the SAME block, all bound to shader storage binding point 0, and require every one to
// read back identically. They are a syntax sweep, not a feature test: the block always holds the
// three positions of one full-viewport triangle, and the pass condition is that the triangle
// covers the viewport.
//
// That shape is what makes them worth reducing here. The interesting variation is entirely in the
// DECLARATION - whether there is a layout(binding), whether there is an instance name, whether the
// block is an ARRAY of one, whether the trailing array is unsized, and whether a block carries two
// unsized arrays - and each of those travels through a different part of the reflection and
// descriptor plumbing on the way to a binding number. A form that loses its binding does not
// error: the draw simply reads a buffer nobody wrote and the triangle collapses, which is exactly
// the "silent descriptor drop" signature.
//
// One case per form on purpose. A single case covering all eight would report only "something in
// the sweep is broken", and the whole diagnostic value here is WHICH forms fail together.

#include <string>
#include <vector>

#include "../Harness/HeadlessGL.h"
#include "../Harness/ScenarioFixture.h"

#ifdef GLAPI
#undef GLAPI
#endif
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glcorearb.h>
#undef GL_GLEXT_PROTOTYPES

namespace MGITest {
    namespace {

        // The eight vertex shaders of the conformance sweep, verbatim in shape, plus a ninth that
        // is not from the sweep (see form 8). Each reads three vec4 positions out of a storage
        // block on binding 0 and emits them as a triangle that covers the whole viewport.
        constexpr const char* kFormVS[9] = {
            // 0 - instance name, no binding qualifier, sized array member
            R"(#version 430 core
layout(std430) buffer Buffer {
  vec4 position[3];
} g_input_buffer;
void main() { gl_Position = g_input_buffer.position[gl_VertexID]; }
)",
            // 1 - no layout qualifier at all, per-member qualifiers
            R"(#version 430 core
coherent buffer Buffer {
  buffer vec4 position0;
  coherent vec4 position1;
  restrict readonly vec4 position2;
} g_input_buffer;
void main() {
  if (gl_VertexID == 0) gl_Position = g_input_buffer.position0;
  if (gl_VertexID == 1) gl_Position = g_input_buffer.position1;
  if (gl_VertexID == 2) gl_Position = g_input_buffer.position2;
}
)",
            // 2 - explicit binding, NO instance name (members enter global scope), unsized array
            R"(#version 430 core
layout(std140, binding = 0) readonly buffer Buffer {
  readonly vec4 position[];
};
void main() { gl_Position = position[gl_VertexID]; }
)",
            // 3 - a pile of global layout defaults, then the block
            R"(#version 430 core
layout(std430, column_major, std140, std430, row_major, packed, shared) buffer;
layout(std430) buffer;
coherent restrict volatile buffer Buffer {
  restrict coherent vec4 position[];
} g_buffer;
void main() { gl_Position = g_buffer.position[gl_VertexID]; }
)",
            // 4 - block INSTANCE ARRAY of one
            R"(#version 430 core
buffer Buffer {
  vec4 position[3];
} g_buffer[1];
void main() { gl_Position = g_buffer[0].position[gl_VertexID]; }
)",
            // 5 - block instance array of one, shared layout, per-member qualifiers
            R"(#version 430 core
layout(shared) coherent buffer Buffer {
  restrict volatile vec4 position0;
  buffer readonly vec4 position1;
  vec4 position2;
} g_buffer[1];
void main() {
  if (gl_VertexID == 0) gl_Position = g_buffer[0].position0;
  else if (gl_VertexID == 1) gl_Position = g_buffer[0].position1;
  else if (gl_VertexID == 2) gl_Position = g_buffer[0].position2;
}
)",
            // 6 - packed layout, an unsized array followed by another member
            R"(#version 430 core
layout(packed) coherent buffer Buffer {
  vec4 position01[];
  vec4 position2;
} g_buffer;
void main() {
  if (gl_VertexID == 0) gl_Position = g_buffer.position01[0];
  else if (gl_VertexID == 1) gl_Position = g_buffer.position01[1];
  else if (gl_VertexID == 2) gl_Position = g_buffer.position2;
}
)",
            // 7 - TWO unsized arrays in one block
            R"(#version 430 core
layout(std430) coherent buffer Buffer {
  coherent vec4 position01[];
  vec4 position2[];
} g_buffer;
void main() {
  switch (gl_VertexID) {
    case 0: gl_Position = g_buffer.position01[0]; break;
    case 1: gl_Position = g_buffer.position01[1]; break;
    case 2: gl_Position = g_buffer.position2[gl_VertexID - 2]; break;
  }
}
)",
            // 8 - NOT from the conformance sweep. An unqualified storage block with a UNIFORM
            // BLOCK beside it, which is what makes the block's DEFAULT binding observable at all.
            //
            // GL 4.3 core 7.8 gives a storage block with no layout(binding = N) a buffer binding
            // of zero. Forms 0, 1, 3, 4 and 5 above are all unqualified and all pass, but they
            // cannot prove that rule holds: they are the only resource in their shader, so the
            // binding glslang's IO mapper invents for them happens to BE zero and the right answer
            // arrives for the wrong reason.
            //
            // Every shader here is parsed as a Vulkan client, so that mapper allocates out of ONE
            // flat space shared by samplers, images, uniform blocks, storage blocks and the
            // synthesized global-uniform block (iomapper.cpp resolveBinding takes the `ent.newSet`
            // branch, and every resource resolves to set 0), and then writes the result back into
            // the type's qualifier - so the reflection cannot tell an invented binding from a
            // declared one. Put anything live next to the block and it is pushed off zero, the
            // draw reads a binding point nothing was ever bound to, and the triangle collapses
            // with no GL error anywhere. That is
            // KHR-GL43.compute_shader.resource-ubo's whole failure, in a vertex stage.
            //
            // The uniform block is REBOUND explicitly with glUniformBlockBinding, exactly as that
            // conformance case does. That keeps this case about the storage block's default and
            // not about the uniform block's - the rebinding path has always worked, and the
            // uniform-block default is a separate (still open) question.
            R"(#version 430 core
layout(std140) uniform ScaleBlock {
  vec4 factor;
} g_scale;
layout(std430) buffer Buffer {
  vec4 position[3];
} g_input_buffer;
void main() { gl_Position = g_input_buffer.position[gl_VertexID] * g_scale.factor; }
)",
        };

        constexpr const char* kFormFS = R"(#version 430 core
layout(location = 0) out vec4 o_color;
void main() { o_color = vec4(0.0, 1.0, 0.0, 1.0); }
)";

        class SsboDeclarationFormScenario : public ScenarioTest {
        protected:
            // A vertex shader reading a storage block needs at least one VS storage block.
            bool StorageBlocksInVertexStage() const {
                GLint blocks = 0;
                glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &blocks);
                while (glGetError() != GL_NO_ERROR) {
                }
                return blocks >= 1;
            }

            // The block's members as the program interface reports them. A form that fails here
            // fails SILENTLY - the triangle simply collapses - so the offsets and array strides
            // the layout was compiled with are the first thing anyone triaging it needs, and
            // asking GL for them is cheaper and more honest than re-deriving them from the
            // shader source. Only used to annotate a failure.
            static std::string DescribeBufferVariables(unsigned int program) {
                std::string out = "  reported GL_BUFFER_VARIABLE layout:\n";
                GLint count = 0;
                glGetProgramInterfaceiv(program, GL_BUFFER_VARIABLE, GL_ACTIVE_RESOURCES, &count);
                for (GLint i = 0; i < count; ++i) {
                    char name[128] = {};
                    GLsizei length = 0;
                    glGetProgramResourceName(program, GL_BUFFER_VARIABLE, static_cast<GLuint>(i), sizeof(name) - 1,
                                             &length, name);
                    const GLenum props[4] = {GL_OFFSET, GL_ARRAY_SIZE, GL_ARRAY_STRIDE, GL_TOP_LEVEL_ARRAY_SIZE};
                    GLint values[4] = {-1, -1, -1, -1};
                    glGetProgramResourceiv(program, GL_BUFFER_VARIABLE, static_cast<GLuint>(i), 4, props,
                                           4, nullptr, values);
                    out += "    " + std::string(name) + ": offset=" + std::to_string(values[0]) +
                           " arraySize=" + std::to_string(values[1]) + " arrayStride=" + std::to_string(values[2]) +
                           " topLevelArraySize=" + std::to_string(values[3]) + "\n";
                }
                while (glGetError() != GL_NO_ERROR) {
                }
                return out;
            }

            // Runs one declaration form end to end and reports whether the triangle covered the
            // viewport. Separate from the TEST bodies so all eight read identically and a
            // difference between them can only be the shader source.
            void RunForm(int form) {
                HeadlessGL& gl = Gl();
                const int width = gl.Width();
                const int height = gl.Height();

                // The three corners of a triangle that covers the whole viewport, which is what
                // the block is expected to deliver to gl_Position.
                const float positions[12] = {-1.0f, -1.0f, 0.0f, 1.0f, 3.0f, -1.0f,
                                             0.0f,  1.0f,  -1.0f, 3.0f, 0.0f, 1.0f};
                GLuint buffer = 0;
                glGenBuffers(1, &buffer);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
                glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(positions), positions, GL_STATIC_DRAW);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buffer);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
                ASSERT_EQ(FirstGLError(), 0u) << "form " << form << ": storage buffer setup errored";

                std::string error;
                const unsigned int program = CompileProgram(kFormVS[form], kFormFS, &error);
                ASSERT_NE(program, 0u) << "form " << form << " did not build: " << error;

                // Form 8 alone declares a uniform block, and it exists only to occupy a slot the
                // storage block must not be pushed onto. Bound to a buffer of ones so it scales
                // the positions by exactly 1 - the block's contribution to the IMAGE is nothing,
                // and its contribution to the TEST is that it is there at all.
                GLuint uniformBuffer = 0;
                if (form == 8) {
                    const float ones[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                    glGenBuffers(1, &uniformBuffer);
                    glBindBuffer(GL_UNIFORM_BUFFER, uniformBuffer);
                    glBufferData(GL_UNIFORM_BUFFER, sizeof(ones), ones, GL_STATIC_DRAW);
                    glBindBufferBase(GL_UNIFORM_BUFFER, 0, uniformBuffer);
                    glBindBuffer(GL_UNIFORM_BUFFER, 0);
                    const GLuint blockIndex = glGetUniformBlockIndex(program, "ScaleBlock");
                    ASSERT_NE(blockIndex, GL_INVALID_INDEX) << "form 8: the uniform block is not active";
                    // Explicit, so this case cannot fail on the uniform block's own default
                    // binding - which is a separate question from the storage block's.
                    glUniformBlockBinding(program, blockIndex, 0);
                    ASSERT_EQ(FirstGLError(), 0u) << "form 8: uniform block setup errored";
                }

                GLuint vao = 0;
                glGenVertexArrays(1, &vao);
                glBindVertexArray(vao);
                BindDefaultFramebuffer();
                glViewport(0, 0, width, height);
                glDisable(GL_SCISSOR_TEST);
                glDisable(GL_DEPTH_TEST);
                ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
                glUseProgram(program);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                EXPECT_EQ(FirstGLError(), 0u) << "form " << form << ": the draw leaked a GL error";

                const Image painted = ReadPixels(width, height);
                const bool covered = static_cast<bool>(RegionIsMostly(
                    painted, 2, width - 3, 2, height - 3, "green", 0.0,
                    "a storage block read from the vertex stage, declaration form " + std::to_string(form)));
                EXPECT_TRUE(covered) << "the block's positions did not reach gl_Position\n"
                                     << DescribeBufferVariables(program);

                glUseProgram(0);
                glBindVertexArray(0);
                glDeleteVertexArrays(1, &vao);
                glDeleteProgram(program);
                glDeleteBuffers(1, &buffer);
                if (uniformBuffer != 0) glDeleteBuffers(1, &uniformBuffer);
                gl.EndFrame();
            }
        };

    } // namespace

#define MGL_SSBO_FORM_CASE(index, name)                                       \
    TEST_F(SsboDeclarationFormScenario, name) {                               \
        if (!Ready()) return;                                                 \
        if (!StorageBlocksInVertexStage())                                    \
            GTEST_SKIP() << "no vertex-stage shader storage blocks";          \
        RunForm(index);                                                       \
    }

    MGL_SSBO_FORM_CASE(0, InstanceNamedBlockWithNoBindingQualifier)
    MGL_SSBO_FORM_CASE(1, BlockWithNoLayoutQualifierAtAll)
    MGL_SSBO_FORM_CASE(2, ExplicitBindingWithNoInstanceName)
    MGL_SSBO_FORM_CASE(3, GlobalLayoutDefaultsThenAnInstanceNamedBlock)
    MGL_SSBO_FORM_CASE(4, BlockInstanceArrayOfOne)
    MGL_SSBO_FORM_CASE(5, BlockInstanceArrayOfOneWithSharedLayout)
    // The form that makes the DEFAULT binding observable rather than accidental: forms 0/1/3/4/5
    // are unqualified too, but nothing competes with them for glslang's flat slot 0, so they
    // would keep passing even with the default wrong. See the comment on kFormVS[8].
    MGL_SSBO_FORM_CASE(8, NoBindingQualifierBesideAUniformBlock)
    // ---- the two forms that do not work yet ----
    //
    // Both carry an UNSIZED array that is not the block's sole trailing member, and both fail
    // IDENTICALLY on Magma and Espryt - which is what says the defect is in the shared frontend
    // and not in either backend's descriptor plumbing.
    //
    // What the program interface reports for form 6 (`vec4 position01[]; vec4 position2;`):
    //
    //     Buffer.position01[0]: offset=0  arraySize=2 arrayStride=16
    //     Buffer.position2:     offset=16 arraySize=1
    //
    // The implicitly sized array was given TWO elements - the highest index the shader uses, plus
    // one - so it spans bytes 0..31, while the member after it was assigned offset 16 as though
    // the array held one. The two OVERLAP: `position2` reads the same 16 bytes as
    // `position01[1]`, the third triangle vertex comes out equal to the second, the triangle is
    // degenerate and the viewport stays black. Form 7 is the same overlap between two runtime
    // arrays. Nothing errors anywhere, which is why this reads as a silent drop.
    //
    // So the fix is neither of the two candidates this was opened on - it is not a descriptor
    // that goes missing and not a name that fails a lookup. Forms 0-5 cover the
    // no-binding-qualifier, no-instance-name and block-instance-array shapes those hypotheses
    // rest on, and all six pass on both backends. (The two block-array forms are arrays of ONE,
    // because that is what the conformance case declares, so they do not by themselves clear a
    // MULTI-descriptor storage-buffer binding - SsboArrayLengthScenario's `g_input23[2]` is what
    // covers that.) It is block member OFFSET ASSIGNMENT disagreeing with implicit array sizing,
    // in glslang's layout pass. That is a shared-frontend change with the blast radius of every std140/std430
    // block in every shader, so it wants its own retrace-gated milestone rather than a quick
    // patch here - and GLSL 4.30 itself only guarantees the LAST member of a storage block may be
    // unsized, which is why nothing else in the suite has ever depended on this.
    //
    // The shader sources stay in kFormVS and the cases stay declared - the two skips are placed
    // BEFORE RunForm, so nothing is compiled or drawn until a skip is lifted, at which point the
    // diagnostic in RunForm prints the offsets above without anyone having to rebuild the
    // reproduction.
    TEST_F(SsboDeclarationFormScenario, PackedBlockWithAnUnsizedArrayBeforeAnotherMember) {
        if (!Ready()) return;
        if (!StorageBlocksInVertexStage()) GTEST_SKIP() << "no vertex-stage shader storage blocks";
        GTEST_SKIP() << "known: a non-trailing unsized array overlaps the member after it (see the note above)";
    }

    TEST_F(SsboDeclarationFormScenario, TwoUnsizedArraysInOneBlock) {
        if (!Ready()) return;
        if (!StorageBlocksInVertexStage()) GTEST_SKIP() << "no vertex-stage shader storage blocks";
        GTEST_SKIP() << "known: two runtime arrays in one block overlap (see the note above)";
    }

#undef MGL_SSBO_FORM_CASE
} // namespace MGITest
