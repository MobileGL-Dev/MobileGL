// MobileGL - MobileGL/MG_Test/Backend/DirectGLES/BaseInstanceInjectionTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// The gate on the gl_BaseInstance indirect lowering in
// MG_Backend/DirectGLES/Managers.cpp. That lowering declares a std430 storage block in the
// VERTEX stage, and a vertex-stage storage block is optional in both APIs: the minimum for
// GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS is 0 (GL 4.6 table 23.64, ES 3.2 table 21.44), and ARM's
// GLES driver takes that allowance - a Mali-G925-Immortalis reports 0 for it and for all three
// other graphics stages.
//
// Emitting the block on such a driver does not make it work. The driver refuses the program at
// link time ("The number of vertex shader storage blocks (1) is greater than the maximum number
// allowed (0)"), and because MobileGL's frontend GL_LINK_STATUS is glslang's rather than the
// driver's, the application is told the program linked and then every draw with it renders
// nothing. Dropping the indirect half instead keeps ordinary draws working and costs only the
// per-command baseInstance of an indirect draw.
//
// No GL context and no driver: the lowering is a pure String -> String pass over one capability.

#include <gtest/gtest.h>

#include <MG_Backend/DirectGLES/DirectGLES.h>
#include <MG_Backend/DirectGLES/Managers.h>

using MobileGL::Bool;
using MobileGL::String;
using MobileGL::MG_Backend::DirectGLES::g_GLESCapabilities;
using MobileGL::MG_Backend::DirectGLES::PromoteDrawParameterGlobalsToUniforms;
using MobileGL::MG_Backend::DirectGLES::VertexStageStorageBlockUsable;

namespace {
    // The capability block is a process-global the backend fills in at init; restore whatever
    // was there so ordering between this suite and any other that touches it cannot matter.
    struct ScopedGLESCapabilitiesOverride {
        ScopedGLESCapabilitiesOverride(): saved(g_GLESCapabilities) {}
        ~ScopedGLESCapabilitiesOverride() { g_GLESCapabilities = saved; }
        ScopedGLESCapabilitiesOverride(const ScopedGLESCapabilitiesOverride&) = delete;
        ScopedGLESCapabilitiesOverride& operator=(const ScopedGLESCapabilitiesOverride&) = delete;

        MobileGL::MG_External::GLESCapabilities saved;
    };

    Bool Contains(const String& haystack, const String& needle) {
        return haystack.find(needle) != String::npos;
    }

    // What SPIRV-Cross hands the backend after LowerDrawParametersPass has demoted
    // gl_BaseInstance to a Private global.
    constexpr const char* kLoweredBaseInstanceVertexShader = R"(#version 310 es
highp int mg_BaseInstanceLowered;
void main() {
    int instance = gl_InstanceID + mg_BaseInstanceLowered;
    gl_Position = vec4(float(instance));
}
)";
} // namespace

// One block is all the indirect view needs, so the predicate is a >= 1 test.
TEST(VertexStageStorageBlockUsableTest, RequiresAtLeastOneBlock) {
    EXPECT_FALSE(VertexStageStorageBlockUsable(0));
    EXPECT_TRUE(VertexStageStorageBlockUsable(1));
    EXPECT_TRUE(VertexStageStorageBlockUsable(16));
}

// A driver that leaves the out-param untouched tells us nothing, and guessing "yes" is exactly
// what produces the unlinkable program. Unusable, not clamped up to one.
TEST(VertexStageStorageBlockUsableTest, ANegativeCountIsUnusableRatherThanClamped) {
    EXPECT_FALSE(VertexStageStorageBlockUsable(-1));
    EXPECT_FALSE(VertexStageStorageBlockUsable(-2147483647 - 1));
}

TEST(BaseInstanceInjectionGate, DriverWithVertexStorageBlocksGetsTheIndirectView) {
    const ScopedGLESCapabilitiesOverride capsGuard;
    g_GLESCapabilities.IndirectDrawInstanceIdIncludesBaseInstance = false;
    g_GLESCapabilities.MaxShaderStorageBufferBindings = 13;
    g_GLESCapabilities.MaxVertexShaderStorageBlocks = 1;

    const String rewritten =
        PromoteDrawParameterGlobalsToUniforms(kLoweredBaseInstanceVertexShader, GL_VERTEX_SHADER);

    EXPECT_TRUE(Contains(rewritten, "layout(std430, binding = 12) readonly buffer mg_IndirectParams"));
    EXPECT_TRUE(Contains(rewritten, "uniform highp int mg_BaseInstanceWordIndex;"));
    EXPECT_TRUE(Contains(rewritten, "#define mg_BaseInstanceLowered ((mg_BaseInstanceWordIndex > 0) ? "
                                    "int(mg_indirectWords[uint(mg_BaseInstanceWordIndex - 1)]) : mg_BaseInstance)"))
        << rewritten;
}

// The bug this gate exists for. The block must not appear at all - not at a different binding,
// not behind a preprocessor guard: a declaration the driver counts is a declaration that makes
// the whole program unlinkable, and the frontend never surfaces that failure.
TEST(BaseInstanceInjectionGate, DriverWithoutVertexStorageBlocksDeclaresNoBlockAtAll) {
    const ScopedGLESCapabilitiesOverride capsGuard;
    g_GLESCapabilities.IndirectDrawInstanceIdIncludesBaseInstance = false;
    g_GLESCapabilities.MaxShaderStorageBufferBindings = 13;
    g_GLESCapabilities.MaxVertexShaderStorageBlocks = 0;

    const String rewritten =
        PromoteDrawParameterGlobalsToUniforms(kLoweredBaseInstanceVertexShader, GL_VERTEX_SHADER);

    EXPECT_FALSE(Contains(rewritten, "mg_IndirectParams")) << rewritten;
    EXPECT_FALSE(Contains(rewritten, "buffer"));
    EXPECT_FALSE(Contains(rewritten, "mg_indirectWords"));
    // Nothing reads the word index any more, so nothing may declare it either - its presence is
    // what BackendProgramObjectImpl uses to decide whether to bind an indirect params buffer.
    EXPECT_FALSE(Contains(rewritten, "mg_BaseInstanceWordIndex"));
}

// Degraded, but still correct for every non-indirect draw: the plain mg_BaseInstance uniform is
// what the non-indirect draw entry points already write.
TEST(BaseInstanceInjectionGate, WithoutTheBlockBaseInstanceFallsBackToThePlainUniform) {
    const ScopedGLESCapabilitiesOverride capsGuard;
    g_GLESCapabilities.IndirectDrawInstanceIdIncludesBaseInstance = false;
    g_GLESCapabilities.MaxShaderStorageBufferBindings = 13;
    g_GLESCapabilities.MaxVertexShaderStorageBlocks = 0;

    const String rewritten =
        PromoteDrawParameterGlobalsToUniforms(kLoweredBaseInstanceVertexShader, GL_VERTEX_SHADER);

    EXPECT_TRUE(Contains(rewritten, "uniform highp int mg_BaseInstance;")) << rewritten;
    EXPECT_TRUE(Contains(rewritten, "#define mg_BaseInstanceLowered (mg_BaseInstance)")) << rewritten;
    // The global declaration must be gone; leaving it would shadow the define.
    EXPECT_FALSE(Contains(rewritten, "highp int mg_BaseInstanceLowered;\n"));
}

// On a driver that both leaks baseInstance into gl_InstanceID and has no vertex storage block,
// the rebase has nothing to subtract. Subtracting the uniform instead would remove the base
// twice from every non-indirect draw, which is worse than not rebasing at all.
TEST(BaseInstanceInjectionGate, WithoutTheBlockInstanceIdRebaseCollapsesToIdentity) {
    const ScopedGLESCapabilitiesOverride capsGuard;
    g_GLESCapabilities.IndirectDrawInstanceIdIncludesBaseInstance = true;
    g_GLESCapabilities.MaxShaderStorageBufferBindings = 13;
    g_GLESCapabilities.MaxVertexShaderStorageBlocks = 0;

    const String rewritten =
        PromoteDrawParameterGlobalsToUniforms(kLoweredBaseInstanceVertexShader, GL_VERTEX_SHADER);

    EXPECT_TRUE(Contains(rewritten, "#define mg_ZeroBasedInstanceID gl_InstanceID")) << rewritten;
    EXPECT_FALSE(Contains(rewritten, "gl_InstanceID - ("));
    EXPECT_FALSE(Contains(rewritten, "mg_indirectWords"));
}

// The gate is scoped to the block, not to the whole pass: mg_DrawID and mg_BaseVertex are plain
// uniforms with no storage block behind them and must still be promoted on such a driver.
TEST(BaseInstanceInjectionGate, DrawIdAndBaseVertexArePromotedRegardless) {
    const ScopedGLESCapabilitiesOverride capsGuard;
    g_GLESCapabilities.IndirectDrawInstanceIdIncludesBaseInstance = false;
    g_GLESCapabilities.MaxShaderStorageBufferBindings = 13;
    g_GLESCapabilities.MaxVertexShaderStorageBlocks = 0;

    const String source = R"(#version 310 es
highp int mg_DrawID;
highp int mg_BaseVertex;
void main() {
    gl_Position = vec4(float(mg_DrawID + mg_BaseVertex));
}
)";

    const String rewritten = PromoteDrawParameterGlobalsToUniforms(source, GL_VERTEX_SHADER);

    EXPECT_TRUE(Contains(rewritten, "uniform highp int mg_DrawID;")) << rewritten;
    EXPECT_TRUE(Contains(rewritten, "uniform highp int mg_BaseVertex;")) << rewritten;
}
