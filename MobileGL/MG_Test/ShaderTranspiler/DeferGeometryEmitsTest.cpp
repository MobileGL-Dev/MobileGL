// MobileGL - MobileGL/MG_Test/ShaderTranspiler/DeferGeometryEmitsTest.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// DeferGeometryEmitsPass moves a geometry stage's emits behind its memory writes for an ES driver
// that drops storage writes made after EmitVertex(). What has to hold: the rewritten module is
// valid, its ESSL calls the original body BEFORE the only EmitVertex() left (the replay), output
// members the shader never writes stay out of the ESSL, and a geometry stage that writes no memory
// is left alone.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "Includes.h"
#include "Init.h"
#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>
#include <MG_Util/ShaderTranspiler/SpvcSession.h>
#include <MG_Util/ShaderTranspiler/Types.h>

#include <spirv-tools/libspirv.hpp>

using namespace MobileGL;
using MobileGL::MG_Util::ShaderTranspiler::SessionUsageBit;
using MobileGL::MG_Util::ShaderTranspiler::ShaderCompiler;
using MobileGL::MG_Util::ShaderTranspiler::SpvcSession;

namespace {
    Vector<Uint32> CompileGeometry(const String& source) {
        using namespace MG_Util::ShaderTranspiler;
        ShaderAttrib shaderAttrib{.shaderType = GL_GEOMETRY_SHADER, .sourceStr = source};
        auto shaderResult = ShaderCompiler::CompileShader(shaderAttrib);
        EXPECT_TRUE(shaderResult) << (shaderResult ? String{} : shaderResult.error().log);
        if (!shaderResult) return {};
        ProgramAttrib programAttrib{.shaders = {shaderResult.value()}};
        auto programResult = ShaderCompiler::LinkProgram(programAttrib);
        EXPECT_TRUE(programResult) << (programResult ? String{} : programResult.error().log);
        if (!programResult) return {};
        ProgramBinaryAttrib binaryAttrib{.shaderTypes = {GL_GEOMETRY_SHADER}, .program = *programResult.value()};
        auto binaryResult = ShaderCompiler::GetSpirvBinaryFromProgram(binaryAttrib);
        EXPECT_TRUE(binaryResult) << (binaryResult ? String{} : binaryResult.error().log);
        if (!binaryResult || binaryResult->empty()) return {};
        return binaryResult->front();
    }

    Bool Validates(const Vector<Uint32>& spirv, String& messages) {
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
        tools.SetMessageConsumer([&messages](spv_message_level_t, const char*, const spv_position_t&,
                                             const char* message) { messages += String(message) + "\n"; });
        return tools.Validate(spirv);
    }

    String Transpile(const Vector<Uint32>& spirv) {
        SpvcSession session(spirv, SessionUsageBit::Transpile);
        auto essl = ShaderCompiler::DecompileShader(session);
        EXPECT_TRUE(essl) << (essl ? String{} : essl.error().log);
        return essl ? essl.value() : String{};
    }

    SizeT Count(const String& haystack, const String& needle) {
        SizeT count = 0;
        for (SizeT at = haystack.find(needle); at != String::npos; at = haystack.find(needle, at + 1))
            ++count;
        return count;
    }

    // KHR-GL4x.shader_storage_buffer_object.basic-atomic-case3's geometry stage: emit first, write after.
    const char* const kWritesAfterEmit = R"(#version 430 core
layout(points) in;
layout(points, max_vertices = 1) out;
layout(std430, binding = 2) buffer GSBuffer { uint u[4]; } g_gs_buffer;
void main() {
    gl_Position = gl_in[0].gl_Position;
    gl_PrimitiveID = gl_PrimitiveIDIn;
    EmitVertex();
    if (atomicAdd(g_gs_buffer.u[gl_PrimitiveIDIn], 1u) != 0u) return;
    atomicAdd(g_gs_buffer.u[0], 2u);
}
)";

    // A strip with a cut in the middle and a user varying, so the replay has to carry a non-block
    // output and reproduce EndPrimitive at the right vertex.
    const char* const kStripWithCut = R"(#version 430 core
layout(points) in;
layout(triangle_strip, max_vertices = 6) out;
layout(std430, binding = 0) buffer Counter { uint n; };
out vec4 color;
void main() {
    for (int i = 0; i < 3; ++i) { gl_Position = vec4(float(i)); color = vec4(1.0); EmitVertex(); }
    EndPrimitive();
    for (int i = 0; i < 3; ++i) { gl_Position = vec4(float(-i)); color = vec4(0.5); EmitVertex(); }
    atomicAdd(n, 1u);
}
)";

    const char* const kNoMemoryWrites = R"(#version 430 core
layout(points) in;
layout(points, max_vertices = 1) out;
void main() { gl_Position = gl_in[0].gl_Position; EmitVertex(); }
)";
} // namespace

TEST(DeferGeometryEmits, WritesAfterEmitAreMovedInFrontOfTheReplay) {
    const Vector<Uint32> spirv = CompileGeometry(kWritesAfterEmit);
    ASSERT_FALSE(spirv.empty());
    Vector<uint32_t> out;
    bool deferred = false;
    ASSERT_TRUE(ShaderCompiler::DeferGeometryEmitsForEssl(spirv, out, deferred, true));
    ASSERT_TRUE(deferred);
    String messages;
    EXPECT_TRUE(Validates(out, messages)) << messages;

    const String essl = Transpile(out);
    ASSERT_FALSE(essl.empty());
    EXPECT_EQ(Count(essl, "EmitVertex()"), 1u) << essl;
    const SizeT call = essl.find("mg_geometryBody();");
    const SizeT emit = essl.find("EmitVertex();");
    ASSERT_NE(call, String::npos) << essl;
    EXPECT_LT(call, emit) << "the body (and its atomics) must run before the replayed emit\n" << essl;
    EXPECT_EQ(essl.find("gl_ClipDistance"), String::npos) << "an unwritten builtin member became active\n" << essl;
    EXPECT_EQ(essl.find("gl_PointSize"), String::npos) << essl;
}

TEST(DeferGeometryEmits, AStripCutAndAUserVaryingAreReplayed) {
    const Vector<Uint32> spirv = CompileGeometry(kStripWithCut);
    ASSERT_FALSE(spirv.empty());
    Vector<uint32_t> out;
    bool deferred = false;
    ASSERT_TRUE(ShaderCompiler::DeferGeometryEmitsForEssl(spirv, out, deferred, true));
    ASSERT_TRUE(deferred);
    String messages;
    EXPECT_TRUE(Validates(out, messages)) << messages;
    const String essl = Transpile(out);
    EXPECT_EQ(Count(essl, "EmitVertex()"), 1u) << essl;
    EXPECT_EQ(Count(essl, "EndPrimitive()"), 1u) << essl;
    EXPECT_NE(essl.find("color = "), String::npos) << essl;
}

TEST(DeferGeometryEmits, AStageThatWritesNoMemoryIsLeftAlone) {
    const Vector<Uint32> spirv = CompileGeometry(kNoMemoryWrites);
    ASSERT_FALSE(spirv.empty());
    Vector<uint32_t> out;
    bool deferred = true;
    ASSERT_TRUE(ShaderCompiler::DeferGeometryEmitsForEssl(spirv, out, deferred, true));
    EXPECT_FALSE(deferred);
}
