// MobileGL - MobileGL/MG_Test/Pipeline/DriverModuleScreenTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// ProgramFactory::ScreenModuleForDriver is the last look Magma takes at a module's bytes before
// vkCreateShaderModule. A driver is not a validating entry point: a module that carried a sampler
// inside the default uniform block (a frontend translation defect) was accepted by Adreno, which
// then page-faulted on a descriptor read out of uniform-buffer bytes and lost the device. The
// screen used to report such a module and hand it over anyway; it must DECLINE it, so the GPU
// never sees it, unless MOBILEGL_MAGMA_ALLOW_INVALID_SPIRV asks otherwise.
//
// The invalid module here is that exact shape, assembled by hand so it stays invalid whatever
// the frontend does: a Block-decorated uniform struct with a sampled-image member.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>

#include "Includes.h"
#include "Init.h"

#include <MG_Backend/DirectVulkan/Renderer/ProgramFactory.h>
#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>
#include <MG_Util/ShaderTranspiler/Types.h>

#include "spirv-tools/libspirv.hpp"

using namespace MobileGL;
using MobileGL::MG_Backend::DirectVulkan::ProgramFactory;
using Verdict = ProgramFactory::DriverModuleVerdict;

namespace {

    // Shared by both modules: a fragment shader whose default uniform block holds a vec4 and,
    // in the invalid one, a sampler2D next to it.
    String UniformBlockFragment(Bool withSampler) {
        String text =
            "OpCapability Shader\n"
            "OpMemoryModel Logical GLSL450\n"
            "OpEntryPoint Fragment %main \"main\" %out\n"
            "OpExecutionMode %main OriginUpperLeft\n"
            "OpDecorate %out Location 0\n"
            "OpDecorate %Block Block\n"
            "OpMemberDecorate %Block 0 Offset 0\n"
            "OpDecorate %ubo DescriptorSet 0\n"
            "OpDecorate %ubo Binding 0\n"
            "%void = OpTypeVoid\n"
            "%fn = OpTypeFunction %void\n"
            "%float = OpTypeFloat 32\n"
            "%v4 = OpTypeVector %float 4\n"
            "%int = OpTypeInt 32 1\n"
            "%zero = OpConstant %int 0\n"
            "%img = OpTypeImage %float 2D 0 0 0 1 Unknown\n"
            "%simg = OpTypeSampledImage %img\n";
        text += withSampler ? "%Block = OpTypeStruct %v4 %simg\n" : "%Block = OpTypeStruct %v4\n";
        text +=
            "%pBlock = OpTypePointer Uniform %Block\n"
            "%ubo = OpVariable %pBlock Uniform\n"
            "%pv4u = OpTypePointer Uniform %v4\n"
            "%pv4o = OpTypePointer Output %v4\n"
            "%out = OpVariable %pv4o Output\n"
            "%main = OpFunction %void None %fn\n"
            "%entry = OpLabel\n"
            "%ptr = OpAccessChain %pv4u %ubo %zero\n"
            "%color = OpLoad %v4 %ptr\n"
            "OpStore %out %color\n"
            "OpReturn\n"
            "OpFunctionEnd\n";
        return text;
    }

    Vector<Uint> Assemble(const String& text) {
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
        std::vector<uint32_t> words;
        EXPECT_TRUE(tools.Assemble(text, &words)) << text;
        return Vector<Uint>(words.begin(), words.end());
    }

    // A realistic module out of the real frontend, for the cost measurement.
    Vector<Uint> CompileFragment(const String& source) {
        using namespace MG_Util::ShaderTranspiler;
        ShaderAttrib shaderAttrib{.shaderType = GL_FRAGMENT_SHADER, .sourceStr = source};
        auto shaderResult = ShaderCompiler::CompileShader(shaderAttrib);
        EXPECT_TRUE(shaderResult) << (shaderResult ? String{} : shaderResult.error().log);
        if (!shaderResult) return {};
        ProgramAttrib programAttrib{.shaders = {shaderResult.value()}};
        auto programResult = ShaderCompiler::LinkProgram(programAttrib);
        EXPECT_TRUE(programResult) << (programResult ? String{} : programResult.error().log);
        if (!programResult) return {};
        ProgramBinaryAttrib binaryAttrib{.shaderTypes = {GL_FRAGMENT_SHADER}, .program = *programResult.value()};
        auto binaryResult = ShaderCompiler::GetSpirvBinaryFromProgram(binaryAttrib);
        if (!binaryResult || binaryResult->empty()) return {};
        const auto& words = binaryResult->front();
        return Vector<Uint>(words.begin(), words.end());
    }

    class DriverModuleScreenTest : public ::testing::Test {
    protected:
        void SetUp() override { MobileGL::Initialize(); }
    };

} // namespace

// The headline: the sampler-in-uniform-block module never reaches the driver. Red before the
// screen acted on its verdict - it answered AdmittedInvalid, i.e. "logged, handed over anyway".
TEST_F(DriverModuleScreenTest, ASamplerInsideTheDefaultUniformBlockIsDeclined) {
    const Vector<Uint> invalid = Assemble(UniformBlockFragment(/*withSampler=*/true));
    ASSERT_FALSE(invalid.empty());
    String reason;
    EXPECT_EQ(ProgramFactory::ScreenModuleForDriver(invalid, /*allowInvalid=*/false, &reason),
              Verdict::DeclinedInvalid);
    EXPECT_FALSE(reason.empty()) << "a decline must carry the validator's reason for its log line";
}

// The positive control: the same block without the sampler validates and is handed over. A screen
// that declined everything would pass the case above and fail this one.
TEST_F(DriverModuleScreenTest, TheSameBlockWithoutTheSamplerPasses) {
    const Vector<Uint> valid = Assemble(UniformBlockFragment(/*withSampler=*/false));
    ASSERT_FALSE(valid.empty());
    String reason = "stale";
    EXPECT_EQ(ProgramFactory::ScreenModuleForDriver(valid, /*allowInvalid=*/false, &reason), Verdict::Valid);
    EXPECT_TRUE(reason.empty());
}

// The escape hatch: with MOBILEGL_MAGMA_ALLOW_INVALID_SPIRV the invalid module is handed over,
// and the verdict says it was invalid rather than pretending it validated.
TEST_F(DriverModuleScreenTest, TheEscapeHatchAdmitsAnInvalidModuleAndSaysSo) {
    const Vector<Uint> invalid = Assemble(UniformBlockFragment(/*withSampler=*/true));
    EXPECT_EQ(ProgramFactory::ScreenModuleForDriver(invalid, /*allowInvalid=*/true), Verdict::AdmittedInvalid);
    const Vector<Uint> valid = Assemble(UniformBlockFragment(/*withSampler=*/false));
    EXPECT_EQ(ProgramFactory::ScreenModuleForDriver(valid, /*allowInvalid=*/true), Verdict::Valid);
}

// The verdict is remembered by content: re-screening the same bytes does not run spirv-val again,
// and a different module does. Also reports what one validation costs next to a cached answer.
TEST_F(DriverModuleScreenTest, TheVerdictIsCachedPerModuleContent) {
    const Vector<Uint> module = CompileFragment(R"(#version 330 core
uniform sampler2D tex0;
uniform sampler2D tex1;
uniform vec4 tint;
uniform mat4 colorMatrix;
in vec2 uv;
in vec4 vertexColor;
out vec4 fragColor;
vec3 tonemap(vec3 c) { return c / (c + vec3(1.0)); }
void main() {
    vec4 base = texture(tex0, uv) * vertexColor;
    vec4 detail = texture(tex1, uv * 4.0);
    vec4 c = colorMatrix * mix(base, base * detail, 0.5) * tint;
    for (int i = 0; i < 4; ++i) c.rgb += 0.01 * sin(c.gbr * float(i + 1));
    fragColor = vec4(tonemap(c.rgb), c.a);
}
)");
    ASSERT_FALSE(module.empty());
    // A copy with one changed word in the bound field of the header: different bytes, same
    // validity, so it must miss the cache.
    Vector<Uint> sibling = module;
    sibling[3] += 1;

    const Uint64 runsBefore = ProgramFactory::DriverModuleValidatorRuns();
    const auto t0 = std::chrono::steady_clock::now();
    const Verdict first = ProgramFactory::ScreenModuleForDriver(module, false);
    const auto t1 = std::chrono::steady_clock::now();
    const Verdict second = ProgramFactory::ScreenModuleForDriver(module, false);
    const auto t2 = std::chrono::steady_clock::now();
    EXPECT_EQ(first, Verdict::Valid);
    EXPECT_EQ(second, first);
    EXPECT_EQ(ProgramFactory::DriverModuleValidatorRuns(), runsBefore + 1) << "the second screen re-validated";

    EXPECT_EQ(ProgramFactory::ScreenModuleForDriver(sibling, false), Verdict::Valid);
    EXPECT_EQ(ProgramFactory::DriverModuleValidatorRuns(), runsBefore + 2) << "different bytes shared a verdict";

    const auto us = [](auto a, auto b) {
        return static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(b - a).count());
    };
    std::printf("[ screen  ] %zu-word module: validate %lld us, cached %lld us\n", module.size(), us(t0, t1),
                us(t1, t2));
}
