// MobileGL - MobileGL/MG_Test/State/NegativeApiErrorsTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The negative-path GL errors the conformance suite checks and MobileGL used to answer
// GL_NO_ERROR to. Every row here is a call the spec requires to fail, lifted from the CTS case
// that found it:
//   * KHR-GL44.multi_bind.errors_bind_buffers / .errors_bind_samplers - ARB_multi_bind's
//     "buffers/samplers will not be created if they do not exist" rule, plus the atomic-counter
//     offset alignment the single-bind path never had.
//   * KHR-GL43.shader_storage_buffer_object.negative-api-bind - the SSBO offset alignment is a
//     property of the binding point and applies with buffer 0 too.
//   * KHR-GL46.indirect_parameters_tests.MultiDraw{Arrays,Elements}IndirectCount - the three
//     errors that guard a parameter-buffer draw.
//   * KHR-GL43.compute_shader.api-indirect / .api-program.
//   * KHR-GLxx.texture_storage.compressed_data - compressed formats on TEXTURE_3D.
// Plus the indexed-getter parity RC-7b is about: glGetBooleani_v / glGetInteger64i_v /
// glGetFloati_v / glGetDoublei_v must answer every pname glGetIntegeri_v answers.
//
// GPU-free: all of it is frontend validation.

#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <vector>

#include "Includes.h"
#include "Init.h"
#include <MG_Impl/GLImpl/Buffer/GL_Buffer.h>
#include <MG_Impl/GLImpl/Drawing/GL_Drawing.h>
#include <MG_Impl/GLImpl/Getter/GL_Getter.h>
#include <MG_Impl/GLImpl/Program/GL_Program.h>
#include <MG_Impl/GLImpl/RenderState/GL_RenderState.h>
#include <MG_Impl/GLImpl/Sampler/GL_Sampler.h>
#include <MG_Impl/GLImpl/Texture/GL_Texture.h>
#include <MG_State/GLState/Core.h>

using namespace MobileGL;
using namespace MobileGL::MG_Impl::GLImpl;

namespace {
    class NegativeApiErrorsTest : public ::testing::Test {
    protected:
        void SetUp() override {
            MobileGL::Initialize();
            MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();
        }

        void TearDown() override {
            EXPECT_EQ(GetError(), GL_NO_ERROR) << "test left an unconsumed GL error behind";
        }

        static void DrainErrors() {
            for (int i = 0; i < 16 && GetError() != GL_NO_ERROR; ++i) {
            }
        }

        static GLuint MakeBuffer(GLenum target, GLsizeiptr size) {
            GLuint buffer = 0;
            GenBuffers(1, &buffer);
            BindBuffer(target, buffer);
            BufferData(target, size, nullptr, GL_STATIC_DRAW);
            return buffer;
        }

        // One table row: run the call, assert exactly the expected error, leave nothing pending.
        struct Row {
            const char* what;
            std::function<void()> call;
            GLenum expected;
        };

        static void RunRows(const std::vector<Row>& rows) {
            for (const Row& row : rows) {
                DrainErrors();
                row.call();
                EXPECT_EQ(GetError(), row.expected) << row.what;
                DrainErrors();
            }
        }
    };

    TEST_F(NegativeApiErrorsTest, MultiBindRejectsNamesThatAreNotObjectsYet) {
        const GLuint buffer = MakeBuffer(GL_UNIFORM_BUFFER, 1024);
        // Reserved by glGenBuffers but never turned into an object: legal for glBindBuffer,
        // which creates it, and illegal for glBindBuffersBase, which must not.
        GLuint reservedOnly = 0;
        GenBuffers(1, &reservedOnly);
        ASSERT_NE(reservedOnly, 0u);
        ASSERT_EQ(IsBuffer(reservedOnly), GL_FALSE);

        GLuint samplerReservedOnly = 0;
        GenSamplers(1, &samplerReservedOnly);
        DrainErrors();

        const GLuint mixedBuffers[2] = {buffer, reservedOnly};
        const GLuint samplers[1] = {samplerReservedOnly};
        const GLintptr offsets[2] = {0, 0};
        const GLsizeiptr sizes[2] = {256, 256};

        RunRows({
            {"glBindBuffersBase with a reserved-but-uncreated name",
             [&] { BindBuffersBase(GL_UNIFORM_BUFFER, 0, 2, mixedBuffers); }, GL_INVALID_OPERATION},
            {"glBindBuffersRange with a reserved-but-uncreated name",
             [&] { BindBuffersRange(GL_UNIFORM_BUFFER, 0, 2, mixedBuffers, offsets, sizes); },
             GL_INVALID_OPERATION},
            {"glBindSamplers with a reserved-but-uncreated name", [&] { BindSamplers(0, 1, samplers); },
             GL_INVALID_OPERATION},
        });

        // The rejected call must have bound nothing at all.
        GLint bound = -1;
        GetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, 0, &bound);
        EXPECT_EQ(bound, 0);
        DrainErrors();
    }

    TEST_F(NegativeApiErrorsTest, BufferRangeOffsetAlignmentAppliesToTheBindingPoint) {
        GLint ssboAlignment = 0;
        GetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &ssboAlignment);
        ASSERT_GT(ssboAlignment, 1) << "the alignment rule is untestable at alignment 1";
        const GLuint atomicBuffer = MakeBuffer(GL_ATOMIC_COUNTER_BUFFER, 1024);
        DrainErrors();

        RunRows({
            // buffer 0 detaches the binding point, but the target's alignment rule still holds.
            {"glBindBufferRange(SHADER_STORAGE_BUFFER, buffer 0, misaligned offset)",
             [&] { BindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, 0, ssboAlignment - 1, 0); }, GL_INVALID_VALUE},
            // An atomic counter binding is addressed in 32-bit counters; it has no queryable
            // alignment pname, which is how its rule went missing.
            {"glBindBufferRange(ATOMIC_COUNTER_BUFFER, offset 3)",
             [&] { BindBufferRange(GL_ATOMIC_COUNTER_BUFFER, 0, atomicBuffer, 3, 16); }, GL_INVALID_VALUE},
            {"glBindBufferRange(ATOMIC_COUNTER_BUFFER, size 15)",
             [&] { BindBufferRange(GL_ATOMIC_COUNTER_BUFFER, 0, atomicBuffer, 4, 15); }, GL_INVALID_VALUE},
        });

        // ...and the aligned form still works.
        DrainErrors();
        BindBufferRange(GL_ATOMIC_COUNTER_BUFFER, 0, atomicBuffer, 4, 16);
        EXPECT_EQ(GetError(), GL_NO_ERROR);
    }

    TEST_F(NegativeApiErrorsTest, DispatchComputeIndirectChecksTheBoundBufferExtent) {
        // Six uints: an indirect dispatch reads three, so offset 16 runs off the end.
        const GLuint dispatchBuffer = MakeBuffer(GL_DISPATCH_INDIRECT_BUFFER, 6 * sizeof(GLuint));
        DrainErrors();

        RunRows({
            {"glDispatchComputeIndirect(-2)", [] { DispatchComputeIndirect(-2); }, GL_INVALID_VALUE},
            {"glDispatchComputeIndirect(3)", [] { DispatchComputeIndirect(3); }, GL_INVALID_VALUE},
            {"glDispatchComputeIndirect(16) past the end of a 24-byte buffer",
             [] { DispatchComputeIndirect(16); }, GL_INVALID_OPERATION},
            {"glDispatchComputeIndirect(0) with nothing bound",
             [&] {
                 BindBuffer(GL_DISPATCH_INDIRECT_BUFFER, 0);
                 DispatchComputeIndirect(0);
             },
             GL_INVALID_OPERATION},
        });
        static_cast<void>(dispatchBuffer);
    }

    TEST_F(NegativeApiErrorsTest, IndirectParameterDrawsCheckBothBuffers) {
        // Two DrawArraysIndirectCommands (16 bytes each) and a roomy parameter buffer.
        MakeBuffer(GL_DRAW_INDIRECT_BUFFER, 2 * 4 * sizeof(GLuint));
        const GLuint parameterBuffer = MakeBuffer(GL_PARAMETER_BUFFER, 200);
        DrainErrors();

        RunRows({
            {"glMultiDrawArraysIndirectCount with drawcount 2 (not a multiple of four)",
             [] { MultiDrawArraysIndirectCount(GL_TRIANGLE_STRIP, nullptr, 2, 1, 0); }, GL_INVALID_VALUE},
            {"glMultiDrawArraysIndirectCount with maxdrawcount past the indirect buffer",
             [] { MultiDrawArraysIndirectCount(GL_TRIANGLE_STRIP, nullptr, 0, 4, 0); }, GL_INVALID_OPERATION},
            {"glMultiDrawElementsIndirectCount with drawcount 2",
             [] { MultiDrawElementsIndirectCount(GL_TRIANGLE_STRIP, GL_UNSIGNED_BYTE, nullptr, 2, 1, 0); },
             GL_INVALID_VALUE},
            {"glMultiDrawArraysIndirectCount with no parameter buffer bound",
             [&] {
                 BindBuffer(GL_PARAMETER_BUFFER, 0);
                 MultiDrawArraysIndirectCount(GL_TRIANGLE_STRIP, nullptr, 0, 2, 0);
             },
             GL_INVALID_OPERATION},
        });
        static_cast<void>(parameterBuffer);
    }

    TEST_F(NegativeApiErrorsTest, TexStorage3DRejectsCompressedFormatsOnTexture3D) {
        GLuint texture = 0;
        GenTextures(1, &texture);
        BindTexture(GL_TEXTURE_3D, texture);
        DrainErrors();

        RunRows({
            {"glTexStorage3D(TEXTURE_3D, GL_COMPRESSED_RED_RGTC1)",
             [] { TexStorage3D(GL_TEXTURE_3D, 1, 0x8DBB /* GL_COMPRESSED_RED_RGTC1 */, 8, 8, 8); },
             GL_INVALID_OPERATION},
            {"glTexStorage3D(TEXTURE_3D, GL_COMPRESSED_RG_RGTC2)",
             [] { TexStorage3D(GL_TEXTURE_3D, 1, 0x8DBD /* GL_COMPRESSED_RG_RGTC2 */, 8, 8, 8); },
             GL_INVALID_OPERATION},
        });

        // An uncompressed sized format on the same target still allocates.
        DrainErrors();
        TexStorage3D(GL_TEXTURE_3D, 1, GL_RGBA8, 8, 8, 8);
        EXPECT_EQ(GetError(), GL_NO_ERROR);
    }

    TEST_F(NegativeApiErrorsTest, LinkRejectsAComputeAndNonComputeMix) {
        const auto attach = [](GLuint program, GLenum stage, const char* source) {
            const GLuint shader = CreateShader(stage);
            ShaderSource(shader, 1, &source, nullptr);
            CompileShader(shader);
            AttachShader(program, shader);
        };
        const GLuint program = CreateProgram();
        attach(program, GL_COMPUTE_SHADER, R"(#version 430 core
layout(local_size_x = 1) in;
layout(std430) buffer Output { uint g_output[]; };
void main() { g_output[gl_GlobalInvocationID.x] = 0; }
)");
        attach(program, GL_VERTEX_SHADER, R"(#version 430 core
layout(location = 0) in vec4 g_position;
void main() { gl_Position = g_position; }
)");
        attach(program, GL_FRAGMENT_SHADER, R"(#version 430 core
layout(location = 0) out vec4 g_color;
void main() { g_color = vec4(1); }
)");
        LinkProgram(program);

        GLint status = GL_TRUE;
        GetProgramiv(program, GL_LINK_STATUS, &status);
        EXPECT_EQ(status, GL_FALSE) << "a compute shader must not link with any other stage";
        DrainErrors();
    }

    // RC-7b: the four non-int indexed getters have to answer the same pname table glGetIntegeri_v
    // does. glGetBooleani_v used to route everything through the indexed-capability path
    // (GL_INVALID_ENUM for anything else) and glGetInteger64i_v straight to the driver, which
    // does not have MobileGL's frontend-only values at all.
    TEST_F(NegativeApiErrorsTest, IndexedGettersAgreeWithGetIntegeriv) {
        DrainErrors();
        const GLenum pnames[] = {GL_MAX_COMPUTE_WORK_GROUP_COUNT, GL_MAX_COMPUTE_WORK_GROUP_SIZE};
        for (GLenum pname : pnames) {
            for (GLuint index = 0; index < 3; ++index) {
                GLint reference = -1;
                GetIntegeri_v(pname, index, &reference);
                ASSERT_EQ(GetError(), GL_NO_ERROR) << "glGetIntegeri_v(" << pname << ", " << index << ")";
                ASSERT_GT(reference, 0) << "the reference value has to be non-trivial to compare against";

                GLint64 as64 = -1;
                GetInteger64i_v(pname, index, &as64);
                EXPECT_EQ(as64, static_cast<GLint64>(reference)) << "glGetInteger64i_v(" << pname << ")";
                EXPECT_EQ(GetError(), GL_NO_ERROR);

                GLfloat asFloat = -1.0f;
                GetFloati_v(pname, index, &asFloat);
                EXPECT_FLOAT_EQ(asFloat, static_cast<GLfloat>(reference)) << "glGetFloati_v(" << pname << ")";
                EXPECT_EQ(GetError(), GL_NO_ERROR);

                GLdouble asDouble = -1.0;
                GetDoublei_v(pname, index, &asDouble);
                EXPECT_DOUBLE_EQ(asDouble, static_cast<GLdouble>(reference)) << "glGetDoublei_v(" << pname << ")";
                EXPECT_EQ(GetError(), GL_NO_ERROR);

                GLboolean asBool = GL_FALSE;
                GetBooleani_v(pname, index, &asBool);
                EXPECT_EQ(asBool, GL_TRUE) << "glGetBooleani_v(" << pname << ")";
                EXPECT_EQ(GetError(), GL_NO_ERROR);
            }
        }
    }

    // ...and the vertex-binding offset keeps its 64-bit width through glGetInteger64i_v, which is
    // how KHR-GL4x.vertex_attrib_binding reads it.
    TEST_F(NegativeApiErrorsTest, VertexBindingOffsetIsReadableThroughTheSixtyFourBitGetter) {
        GLuint vao = 0;
        GenVertexArrays(1, &vao);
        BindVertexArray(vao);
        const GLuint vbo = MakeBuffer(GL_ARRAY_BUFFER, 4096);
        DrainErrors();

        GLint64 offset = -1;
        GetInteger64i_v(GL_VERTEX_BINDING_OFFSET, 0, &offset);
        EXPECT_EQ(offset, 0);
        EXPECT_EQ(GetError(), GL_NO_ERROR);

        BindVertexBuffer(0, vbo, 2048, 128);
        GetInteger64i_v(GL_VERTEX_BINDING_OFFSET, 0, &offset);
        EXPECT_EQ(offset, 2048);
        EXPECT_EQ(GetError(), GL_NO_ERROR);
    }
} // namespace
