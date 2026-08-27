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
//   * KHR-GL32.api.coverage - glFenceSync's condition/flags and glWaitSync's flags/timeout.
//   * KHR-GL31.api.coverage - a draw's mode INVALID_ENUM has to outrank MobileGL's own
//     no-current-program guard.
//   * KHR-GL30.api.coverage - glBlitFramebuffer's mask bits, filter enum and the LINEAR-with-
//     depth/stencil rule.
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
#include <MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.h>
#include <MG_Impl/GLImpl/Getter/GL_Getter.h>
#include <MG_Impl/GLImpl/Program/GL_Program.h>
#include <MG_Impl/GLImpl/RenderState/GL_RenderState.h>
#include <MG_Impl/GLImpl/Sampler/GL_Sampler.h>
#include <MG_Impl/GLImpl/Sync/GL_Sync.h>
#include <MG_Impl/GLImpl/Texture/GL_Texture.h>
#include <MG_Impl/GLImpl/VertexArray/GL_VertexArray.h>
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

        // glGenSamplers, unlike glGenBuffers, creates the objects outright, so a sampler name is
        // only "not an existing object" once it has been deleted.
        GLuint deadSampler = 0;
        GenSamplers(1, &deadSampler);
        ASSERT_NE(deadSampler, 0u);
        DeleteSamplers(1, &deadSampler);
        DrainErrors();

        const GLuint mixedBuffers[2] = {buffer, reservedOnly};
        const GLuint samplers[1] = {deadSampler};
        const GLintptr offsets[2] = {0, 0};
        const GLsizeiptr sizes[2] = {256, 256};

        RunRows({
            {"glBindBuffersBase with a reserved-but-uncreated name",
             [&] { BindBuffersBase(GL_UNIFORM_BUFFER, 0, 2, mixedBuffers); }, GL_INVALID_OPERATION},
            {"glBindBuffersRange with a reserved-but-uncreated name",
             [&] { BindBuffersRange(GL_UNIFORM_BUFFER, 0, 2, mixedBuffers, offsets, sizes); },
             GL_INVALID_OPERATION},
            {"glBindSamplers with a deleted sampler name", [&] { BindSamplers(0, 1, samplers); },
             GL_INVALID_OPERATION},
        });

        // ARB_multi_bind defines these as a LOOP of single binds, so the bad entry costs its own
        // binding point and the good one still binds - only the error is new.
        GLint bound = -1;
        GetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, 0, &bound);
        EXPECT_EQ(static_cast<GLuint>(bound), buffer) << "a rejected element must not take the valid ones with it";
        GetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, 1, &bound);
        EXPECT_EQ(bound, 0) << "the rejected element must not have bound anything";
        DrainErrors();
    }

    // KHR-GL44.multi_bind.errors_bind_textures / .errors_bind_image_textures / .errors_bind_samplers.
    // Both entry points were silent no-op stubs, so every row here answered GL_NO_ERROR.
    // errors_bind_samplers is in the list because that case checks the invalid-name rule by calling
    // glBindTextures with a sampler-name array - a name from the wrong namespace is simply not an
    // existing texture.
    TEST_F(NegativeApiErrorsTest, MultiBindTexturesRejectsBadRangesAndNames) {
        GLint maxUnits = 0;
        GetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &maxUnits);
        ASSERT_GT(maxUnits, 0);
        GLint maxImageUnits = 0;
        GetIntegerv(GL_MAX_IMAGE_UNITS, &maxImageUnits);

        GLuint texture = 0;
        GenTextures(1, &texture);
        BindTexture(GL_TEXTURE_2D, texture);
        TexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 4, 4);

        // Reserved by glGenTextures but never bound: not an object yet, so the multi-bind entry
        // points must refuse it instead of creating it the way glBindTexture would.
        GLuint reservedOnly = 0;
        GenTextures(1, &reservedOnly);
        ASSERT_NE(reservedOnly, 0u);
        ASSERT_EQ(IsTexture(reservedOnly), GL_FALSE);
        DrainErrors();

        const GLuint good[1] = {texture};
        const GLuint mixed[2] = {texture, reservedOnly};

        std::vector<Row> rows = {
            {"glBindTextures with negative count", [&] { BindTextures(0, -1, good); }, GL_INVALID_VALUE},
            {"glBindTextures with first + count past the last unit",
             [&] { BindTextures(static_cast<GLuint>(maxUnits), 1, good); }, GL_INVALID_OPERATION},
            {"glBindTextures with a reserved-but-uncreated name", [&] { BindTextures(0, 2, mixed); },
             GL_INVALID_OPERATION},
            {"glBindImageTextures with negative count", [&] { BindImageTextures(0, -1, good); }, GL_INVALID_VALUE},
        };
        if (maxImageUnits > 0) {
            rows.push_back({"glBindImageTextures with first + count past the last image unit",
                            [&] { BindImageTextures(static_cast<GLuint>(maxImageUnits), 1, good); },
                            GL_INVALID_OPERATION});
            rows.push_back({"glBindImageTextures with a reserved-but-uncreated name",
                            [&] { BindImageTextures(0, 2, mixed); }, GL_INVALID_OPERATION});
        }
        RunRows(rows);

        // The loop semantics again: the good element at index 0 binds, the bad one does not.
        GLint bound = -1;
        GetIntegeri_v(GL_TEXTURE_BINDING_2D, 0, &bound);
        EXPECT_EQ(static_cast<GLuint>(bound), texture) << "a rejected element must not take the valid ones with it";
        GetIntegeri_v(GL_TEXTURE_BINDING_2D, 1, &bound);
        EXPECT_EQ(bound, 0) << "the rejected element must not have bound anything";
        DrainErrors();
    }

    // KHR-GL44.multi_bind.functional_bind_textures / .functional_bind_image_textures: the binding
    // has to land on the texture's OWN target - glBindTextures takes no target parameter - and
    // element zero has to unbind every target of its unit.
    TEST_F(NegativeApiErrorsTest, MultiBindTexturesBindsToTheTexturesOwnTarget) {
        GLuint textures[2] = {0, 0};
        GenTextures(2, textures);
        BindTexture(GL_TEXTURE_1D, textures[0]);
        TexStorage1D(GL_TEXTURE_1D, 1, GL_RGBA8, 4);
        BindTexture(GL_TEXTURE_3D, textures[1]);
        TexStorage3D(GL_TEXTURE_3D, 1, GL_RGBA8, 4, 4, 4);
        // Leave the active unit's slots clean so only the multi-bind result is under test.
        BindTexture(GL_TEXTURE_1D, 0);
        BindTexture(GL_TEXTURE_3D, 0);
        DrainErrors();

        BindTextures(0, 2, textures);
        EXPECT_EQ(GetError(), GL_NO_ERROR);

        GLint bound = -1;
        GetIntegeri_v(GL_TEXTURE_BINDING_1D, 0, &bound);
        EXPECT_EQ(static_cast<GLuint>(bound), textures[0]) << "a 1D texture must land on the unit's 1D slot";
        GetIntegeri_v(GL_TEXTURE_BINDING_3D, 0, &bound);
        EXPECT_EQ(bound, 0) << "no other target of the unit may be touched";
        GetIntegeri_v(GL_TEXTURE_BINDING_3D, 1, &bound);
        EXPECT_EQ(static_cast<GLuint>(bound), textures[1]) << "a 3D texture must land on the unit's 3D slot";

        // A zero element - and a NULL array - unbind EVERY target of the unit, not just one.
        const GLuint zeros[1] = {0};
        BindTextures(0, 1, zeros);
        GetIntegeri_v(GL_TEXTURE_BINDING_1D, 0, &bound);
        EXPECT_EQ(bound, 0);
        BindTextures(1, 1, nullptr);
        GetIntegeri_v(GL_TEXTURE_BINDING_3D, 1, &bound);
        EXPECT_EQ(bound, 0) << "a NULL <textures> unbinds the range";
        EXPECT_EQ(GetError(), GL_NO_ERROR);

        GLint maxImageUnits = 0;
        GetIntegerv(GL_MAX_IMAGE_UNITS, &maxImageUnits);
        if (maxImageUnits > 0) {
            // ARB_multi_bind fixes every glBindImageTexture parameter but the unit and the name:
            // level 0, layered, layer 0, READ_WRITE, and the texture's own internal format.
            BindImageTextures(0, 1, &textures[1]);
            EXPECT_EQ(GetError(), GL_NO_ERROR);
            GetIntegeri_v(GL_IMAGE_BINDING_NAME, 0, &bound);
            EXPECT_EQ(static_cast<GLuint>(bound), textures[1]);
            GetIntegeri_v(GL_IMAGE_BINDING_LEVEL, 0, &bound);
            EXPECT_EQ(bound, 0);
            GetIntegeri_v(GL_IMAGE_BINDING_LAYERED, 0, &bound);
            EXPECT_EQ(bound, GL_TRUE);
            GetIntegeri_v(GL_IMAGE_BINDING_ACCESS, 0, &bound);
            EXPECT_EQ(bound, GL_READ_WRITE);
            GetIntegeri_v(GL_IMAGE_BINDING_FORMAT, 0, &bound);
            EXPECT_EQ(bound, GL_RGBA8);

            BindImageTextures(0, 1, nullptr);
            GetIntegeri_v(GL_IMAGE_BINDING_NAME, 0, &bound);
            EXPECT_EQ(bound, 0) << "a NULL <textures> resets the image unit";
        }

        // Through the EXPORTED entry points, not just the GLImpl functions: both of these were
        // declared with the stub macro, so a working implementation that is never wired into
        // Definitions.cpp still answers GL_NO_ERROR and binds nothing.
        ::glBindTextures(0, 1, &textures[0]);
        GetIntegeri_v(GL_TEXTURE_BINDING_1D, 0, &bound);
        EXPECT_EQ(static_cast<GLuint>(bound), textures[0]) << "glBindTextures is still exported as a no-op stub";
        if (maxImageUnits > 0) {
            ::glBindImageTextures(0, 1, &textures[1]);
            GetIntegeri_v(GL_IMAGE_BINDING_NAME, 0, &bound);
            EXPECT_EQ(static_cast<GLuint>(bound), textures[1])
                << "glBindImageTextures is still exported as a no-op stub";
        }
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

    // A transform feedback name has two different truths and glDrawTransformFeedback used to ask
    // for the wrong one. glGenTransformFeedbacks only RESERVES a name; the first
    // glBindTransformFeedback is what creates the object (GL 4.6 core 13.2.1), and
    // glIsTransformFeedback reports exactly that distinction. glDrawTransformFeedback's
    // "id is not the name of a transform feedback object" INVALID_VALUE has to agree with
    // glIsTransformFeedback, or a caller that picks an unused name the way
    // KHR-GL4x.transform_feedback.api_errors_test does - increment until glIsTransformFeedback
    // says false - gets a name the draw then accepts, and the draw falls through to a different
    // error entirely (INVALID_OPERATION, "glEndTransformFeedback has never been called").
    //
    // The draw path itself needs a backend and a linked program before it reaches the name, which
    // this GPU-free suite has neither of, so what is pinned here is the predicate pair the fix
    // turns on: the two must not collapse back into one.
    TEST_F(NegativeApiErrorsTest, ReservedTransformFeedbackNameIsNotYetAnObject) {
        GLuint name = 0;
        GenTransformFeedbacks(1, &name);
        ASSERT_NE(name, 0u);
        DrainErrors();

        // Reserved, so it is a legal argument to glBindTransformFeedback...
        EXPECT_TRUE(MG_State::pGLContext->ValidateTransformFeedbackName(name));
        // ...but not an object yet, which is what a draw must key off.
        EXPECT_FALSE(MG_State::pGLContext->IsTransformFeedbackObject(name));
        EXPECT_EQ(IsTransformFeedback(name), GL_FALSE);

        BindTransformFeedback(GL_TRANSFORM_FEEDBACK, name);
        EXPECT_EQ(GetError(), GL_NO_ERROR);

        EXPECT_TRUE(MG_State::pGLContext->ValidateTransformFeedbackName(name));
        EXPECT_TRUE(MG_State::pGLContext->IsTransformFeedbackObject(name));
        EXPECT_EQ(IsTransformFeedback(name), GL_TRUE);

        // The default object is never "an object" by this predicate and is always drawable, so
        // the draw path has to special-case it rather than reuse the answer directly.
        EXPECT_FALSE(MG_State::pGLContext->IsTransformFeedbackObject(0));
        EXPECT_TRUE(MG_State::pGLContext->ValidateTransformFeedbackName(0));

        BindTransformFeedback(GL_TRANSFORM_FEEDBACK, 0);
        DrainErrors();
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

    // KHR-GL32.api.coverage: glFenceSync and glWaitSync took every argument they were handed and
    // reported GL_NO_ERROR for the two calls GL 4.6 core 4.1.2 requires to fail. A rejected
    // glFenceSync must also hand back 0 rather than a live handle.
    TEST_F(NegativeApiErrorsTest, SyncEntryPointsRejectTheirIllegalArguments) {
        DrainErrors();

        RunRows({
            {"glFenceSync with a condition other than GL_SYNC_GPU_COMMANDS_COMPLETE",
             [] { EXPECT_EQ(FenceSync(GL_SYNC_FENCE, 0), nullptr); }, GL_INVALID_ENUM},
            {"glFenceSync with nonzero flags", [] { EXPECT_EQ(FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 1), nullptr); },
             GL_INVALID_VALUE},
        });

        // The legal fence still works, and with no backend function table it is the always-signaled
        // fallback - which is all this GPU-free suite needs to reach glWaitSync's own checks.
        const GLsync sync = FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        ASSERT_NE(sync, nullptr);
        EXPECT_EQ(GetError(), GL_NO_ERROR);
        EXPECT_EQ(IsSync(sync), GL_TRUE);

        RunRows({
            {"glWaitSync with nonzero flags", [&] { WaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED); },
             GL_INVALID_VALUE},
            {"glWaitSync with a finite timeout", [&] { WaitSync(sync, 0, 1000000000ull); }, GL_INVALID_VALUE},
            {"glWaitSync with the only legal argument pair", [&] { WaitSync(sync, 0, GL_TIMEOUT_IGNORED); },
             GL_NO_ERROR},
        });

        DeleteSync(sync);
        EXPECT_EQ(GetError(), GL_NO_ERROR);
    }

    // KHR-GL31.api.coverage's first two calls are glDrawArraysInstanced / glDrawElementsInstanced
    // with mode GL_POINTS-1 against a context that has no program and no VAO bound, and they must
    // answer GL_INVALID_ENUM. MobileGL's own "there is no current program" guard - which the spec
    // does not list as a draw error at all - used to run first and shadowed the enum check with
    // GL_INVALID_OPERATION. Nothing here reaches a backend: the mode is rejected before the guard.
    TEST_F(NegativeApiErrorsTest, BadPrimitiveModeOutranksTheNoProgramGuard) {
        DrainErrors();
        // Exactly what the coverage test passes: GL_POINTS is 0, so this is 0xFFFFFFFF.
        constexpr GLenum kBadMode = static_cast<GLenum>(GL_POINTS - 1);

        RunRows({
            {"glDrawArraysInstanced with an unaccepted mode", [] { DrawArraysInstanced(kBadMode, 0, 3, 4); },
             GL_INVALID_ENUM},
            {"glDrawElementsInstanced with an unaccepted mode",
             [] { DrawElementsInstanced(kBadMode, 3, GL_UNSIGNED_INT, nullptr, 4); }, GL_INVALID_ENUM},
            {"glDrawArrays with an unaccepted mode", [] { DrawArrays(kBadMode, 0, 3); }, GL_INVALID_ENUM},
            {"glDrawElements with an unaccepted mode",
             [] { DrawElements(kBadMode, 3, GL_UNSIGNED_INT, nullptr); }, GL_INVALID_ENUM},
            {"glMultiDrawArrays with an unaccepted mode",
             [] { MultiDrawArrays(kBadMode, nullptr, nullptr, 0); }, GL_INVALID_ENUM},
            {"glDrawRangeElements with an unaccepted mode",
             [] { DrawRangeElements(kBadMode, 0, 2, 3, GL_UNSIGNED_INT, nullptr); }, GL_INVALID_ENUM},
            {"glDrawElementsIndirect with an unaccepted mode",
             [] { DrawElementsIndirect(kBadMode, GL_UNSIGNED_INT, nullptr); }, GL_INVALID_ENUM},
            {"glDrawArraysIndirect with an unaccepted mode", [] { DrawArraysIndirect(kBadMode, nullptr); },
             GL_INVALID_ENUM},
            // A mode the enum check accepts falls through to the no-program path, which is now
            // a SILENT drop rather than an error: GL 4.6 core 7.3 and ES 3.1 7.3 both make a draw
            // with no current program and no bound pipeline UNDEFINED, not erroneous, and
            // es31cSeparateShaderObjsTests.StateInteraction reads glGetError() straight after
            // useProgram(0) + bindProgramPipeline(0) + glDrawElements and requires GL_NO_ERROR.
            // Dropping the draw is one of the shapes "undefined" may take; inventing an error is
            // not. The enum check above still outranks it, which is what this case is really for.
            {"glDrawArrays with a legal mode and no program bound", [] { DrawArrays(GL_TRIANGLES, 0, 3); },
             GL_NO_ERROR},
        });
    }

    // KHR-GL30.api.coverage's glBlitFramebuffer sub-check. The frontend passed mask and filter
    // straight through, and DirectGLES drains the driver's error queue around the blit so the ES
    // rejection never surfaced either - both illegal calls reported GL_NO_ERROR. Every row here
    // is rejected before the backend function pointer is reached, which is what lets this
    // GPU-free suite run them at all.
    TEST_F(NegativeApiErrorsTest, BlitFramebufferRejectsBadMasksAndFilters) {
        DrainErrors();
        // The bit the coverage test smuggles in: a legal glMapBufferRange flag, not a blit one.
        constexpr GLbitfield kForeignBit = GL_MAP_INVALIDATE_BUFFER_BIT;

        RunRows({
            {"glBlitFramebuffer with a mask bit outside COLOR|DEPTH|STENCIL",
             [] {
                 BlitFramebuffer(0, 0, 16, 16, 0, 0, 16, 16, GL_COLOR_BUFFER_BIT | kForeignBit, GL_NEAREST);
             },
             GL_INVALID_VALUE},
            {"glBlitFramebuffer with a filter that is neither GL_NEAREST nor GL_LINEAR",
             [] { BlitFramebuffer(0, 0, 16, 16, 0, 0, 16, 16, GL_COLOR_BUFFER_BIT, GL_NONE); }, GL_INVALID_ENUM},
            {"glBlitFramebuffer of colour+stencil with GL_LINEAR",
             [] {
                 BlitFramebuffer(0, 0, 16, 16, 0, 0, 16, 16, GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_LINEAR);
             },
             GL_INVALID_OPERATION},
            {"glBlitFramebuffer of depth with GL_LINEAR",
             [] { BlitFramebuffer(0, 0, 16, 16, 0, 0, 16, 16, GL_DEPTH_BUFFER_BIT, GL_LINEAR); },
             GL_INVALID_OPERATION},
            // The DSA form has to answer identically.
            {"glBlitNamedFramebuffer with a mask bit outside COLOR|DEPTH|STENCIL",
             [] {
                 BlitNamedFramebuffer(0, 0, 0, 0, 16, 16, 0, 0, 16, 16, GL_COLOR_BUFFER_BIT | kForeignBit,
                                      GL_NEAREST);
             },
             GL_INVALID_VALUE},
            {"glBlitNamedFramebuffer with a bad filter",
             [] { BlitNamedFramebuffer(0, 0, 0, 0, 16, 16, 0, 0, 16, 16, GL_COLOR_BUFFER_BIT, GL_NONE); },
             GL_INVALID_ENUM},
            {"glBlitNamedFramebuffer of depth+stencil with GL_LINEAR",
             [] {
                 BlitNamedFramebuffer(0, 0, 0, 0, 16, 16, 0, 0, 16, 16,
                                      GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_LINEAR);
             },
             GL_INVALID_OPERATION},
        });
    }
} // namespace
