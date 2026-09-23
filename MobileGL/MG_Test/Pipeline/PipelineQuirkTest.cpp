// MobileGL - MobileGL/MG_Test/Pipeline/PipelineQuirkTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <gtest/gtest.h>

#include <Config.h>
#include <MG_Backend/DirectVulkan/Renderer/PipelineFactory.h>
#include <MG_Backend/DirectVulkan/Renderer/ProgramFactory.h>
#if MOBILEGL_BUILD_DISAGGREGATED
#include <MG_Backend/DirectVulkan/Renderer/WireRenderPassCompatibility.h>
#include <MG_Backend/DirectVulkan/Renderer/WireDepthResolveArm.h>
#endif

using namespace MobileGL;
using MobileGL::MG_Backend::DirectVulkan::PipelineFactory;
using MobileGL::MG_Backend::DirectVulkan::ProgramFactory;
using MobileGL::MG_Config::QuirkOverride;

namespace {
    constexpr Uint32 kVendorIdQualcomm = 0x5143;
    constexpr Uint32 kVendorIdArm = 0x13B5;

    constexpr VkColorComponentFlags kFullColorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    // Builds non-separate blend state: the alpha channel repeats the color factors/op, which
    // is what glBlendFunc/glBlendEquation (as opposed to their *Separate forms) produce.
    // ShouldSuppressDepthWrite deliberately decides on the color channel alone, so these
    // cases cover its whole input space; SeparateAlphaAccumulationIsNotStripped below pins
    // the separate-alpha contract.
    VkPipelineColorBlendAttachmentState MakeBlendAttachment(Bool blendEnable,
                                                            VkBlendFactor srcColor,
                                                            VkBlendFactor dstColor,
                                                            VkBlendOp colorOp,
                                                            VkColorComponentFlags colorWriteMask) {
        VkPipelineColorBlendAttachmentState attachment{};
        attachment.blendEnable = blendEnable ? VK_TRUE : VK_FALSE;
        attachment.srcColorBlendFactor = srcColor;
        attachment.dstColorBlendFactor = dstColor;
        attachment.colorBlendOp = colorOp;
        attachment.srcAlphaBlendFactor = srcColor;
        attachment.dstAlphaBlendFactor = dstColor;
        attachment.alphaBlendOp = colorOp;
        attachment.colorWriteMask = colorWriteMask;
        return attachment;
    }

    // glslangValidator -V output for:
    //   #version 450
    //   layout(location = 0) out vec4 outColor;
    //   void main() { outColor = vec4(1.0); gl_FragDepth = 0.5; }
    // Assigning gl_FragDepth makes glslang emit OpExecutionMode ... DepthReplacing.
    constexpr Uint32 kFragDepthWriterSpirv[] = {
        0x07230203u, 0x00010000u, 0x0008000bu, 0x0000000fu, 0x00000000u, 0x00020011u,
        0x00000001u, 0x0006000bu, 0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu,
        0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u, 0x0007000fu, 0x00000004u,
        0x00000004u, 0x6e69616du, 0x00000000u, 0x00000009u, 0x0000000du, 0x00030010u,
        0x00000004u, 0x00000007u, 0x00030010u, 0x00000004u, 0x0000000cu, 0x00030003u,
        0x00000002u, 0x000001c2u, 0x00040005u, 0x00000004u, 0x6e69616du, 0x00000000u,
        0x00050005u, 0x00000009u, 0x4374756fu, 0x726f6c6fu, 0x00000000u, 0x00060005u,
        0x0000000du, 0x465f6c67u, 0x44676172u, 0x68747065u, 0x00000000u, 0x00040047u,
        0x00000009u, 0x0000001eu, 0x00000000u, 0x00040047u, 0x0000000du, 0x0000000bu,
        0x00000016u, 0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u,
        0x00030016u, 0x00000006u, 0x00000020u, 0x00040017u, 0x00000007u, 0x00000006u,
        0x00000004u, 0x00040020u, 0x00000008u, 0x00000003u, 0x00000007u, 0x0004003bu,
        0x00000008u, 0x00000009u, 0x00000003u, 0x0004002bu, 0x00000006u, 0x0000000au,
        0x3f800000u, 0x0007002cu, 0x00000007u, 0x0000000bu, 0x0000000au, 0x0000000au,
        0x0000000au, 0x0000000au, 0x00040020u, 0x0000000cu, 0x00000003u, 0x00000006u,
        0x0004003bu, 0x0000000cu, 0x0000000du, 0x00000003u, 0x0004002bu, 0x00000006u,
        0x0000000eu, 0x3f000000u, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u,
        0x00000003u, 0x000200f8u, 0x00000005u, 0x0003003eu, 0x00000009u, 0x0000000bu,
        0x0003003eu, 0x0000000du, 0x0000000eu, 0x000100fdu, 0x00010038u,
    };

    // Same shader without the gl_FragDepth assignment.
    constexpr Uint32 kPlainFragmentSpirv[] = {
        0x07230203u, 0x00010000u, 0x0008000bu, 0x0000000cu, 0x00000000u, 0x00020011u,
        0x00000001u, 0x0006000bu, 0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu,
        0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u, 0x0006000fu, 0x00000004u,
        0x00000004u, 0x6e69616du, 0x00000000u, 0x00000009u, 0x00030010u, 0x00000004u,
        0x00000007u, 0x00030003u, 0x00000002u, 0x000001c2u, 0x00040005u, 0x00000004u,
        0x6e69616du, 0x00000000u, 0x00050005u, 0x00000009u, 0x4374756fu, 0x726f6c6fu,
        0x00000000u, 0x00040047u, 0x00000009u, 0x0000001eu, 0x00000000u, 0x00020013u,
        0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u, 0x00030016u, 0x00000006u,
        0x00000020u, 0x00040017u, 0x00000007u, 0x00000006u, 0x00000004u, 0x00040020u,
        0x00000008u, 0x00000003u, 0x00000007u, 0x0004003bu, 0x00000008u, 0x00000009u,
        0x00000003u, 0x0004002bu, 0x00000006u, 0x0000000au, 0x3f800000u, 0x0007002cu,
        0x00000007u, 0x0000000bu, 0x0000000au, 0x0000000au, 0x0000000au, 0x0000000au,
        0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u, 0x000200f8u,
        0x00000005u, 0x0003003eu, 0x00000009u, 0x0000000bu, 0x000100fdu, 0x00010038u,
    };


    // glslangValidator -V output for a vertex shader reading gl_InstanceIndex:
    //   #version 450
    //   layout(location = 0) in vec4 inPos;
    //   void main() { gl_Position = inPos + vec4(float(gl_InstanceIndex)); }
    constexpr Uint32 kInstanceIndexVertexSpirv[] = {
        0x07230203u, 0x00010000u, 0x0008000bu, 0x0000001bu, 0x00000000u, 0x00020011u,
        0x00000001u, 0x0006000bu, 0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu,
        0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u, 0x0008000fu, 0x00000000u,
        0x00000004u, 0x6e69616du, 0x00000000u, 0x0000000du, 0x00000011u, 0x00000014u,
        0x00030003u, 0x00000002u, 0x000001c2u, 0x00040005u, 0x00000004u, 0x6e69616du,
        0x00000000u, 0x00060005u, 0x0000000bu, 0x505f6c67u, 0x65567265u, 0x78657472u,
        0x00000000u, 0x00060006u, 0x0000000bu, 0x00000000u, 0x505f6c67u, 0x7469736fu,
        0x006e6f69u, 0x00070006u, 0x0000000bu, 0x00000001u, 0x505f6c67u, 0x746e696fu,
        0x657a6953u, 0x00000000u, 0x00070006u, 0x0000000bu, 0x00000002u, 0x435f6c67u,
        0x4470696cu, 0x61747369u, 0x0065636eu, 0x00070006u, 0x0000000bu, 0x00000003u,
        0x435f6c67u, 0x446c6c75u, 0x61747369u, 0x0065636eu, 0x00030005u, 0x0000000du,
        0x00000000u, 0x00040005u, 0x00000011u, 0x6f506e69u, 0x00000073u, 0x00070005u,
        0x00000014u, 0x495f6c67u, 0x6174736eu, 0x4965636eu, 0x7865646eu, 0x00000000u,
        0x00030047u, 0x0000000bu, 0x00000002u, 0x00050048u, 0x0000000bu, 0x00000000u,
        0x0000000bu, 0x00000000u, 0x00050048u, 0x0000000bu, 0x00000001u, 0x0000000bu,
        0x00000001u, 0x00050048u, 0x0000000bu, 0x00000002u, 0x0000000bu, 0x00000003u,
        0x00050048u, 0x0000000bu, 0x00000003u, 0x0000000bu, 0x00000004u, 0x00040047u,
        0x00000011u, 0x0000001eu, 0x00000000u, 0x00040047u, 0x00000014u, 0x0000000bu,
        0x0000002bu, 0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u,
        0x00030016u, 0x00000006u, 0x00000020u, 0x00040017u, 0x00000007u, 0x00000006u,
        0x00000004u, 0x00040015u, 0x00000008u, 0x00000020u, 0x00000000u, 0x0004002bu,
        0x00000008u, 0x00000009u, 0x00000001u, 0x0004001cu, 0x0000000au, 0x00000006u,
        0x00000009u, 0x0006001eu, 0x0000000bu, 0x00000007u, 0x00000006u, 0x0000000au,
        0x0000000au, 0x00040020u, 0x0000000cu, 0x00000003u, 0x0000000bu, 0x0004003bu,
        0x0000000cu, 0x0000000du, 0x00000003u, 0x00040015u, 0x0000000eu, 0x00000020u,
        0x00000001u, 0x0004002bu, 0x0000000eu, 0x0000000fu, 0x00000000u, 0x00040020u,
        0x00000010u, 0x00000001u, 0x00000007u, 0x0004003bu, 0x00000010u, 0x00000011u,
        0x00000001u, 0x00040020u, 0x00000013u, 0x00000001u, 0x0000000eu, 0x0004003bu,
        0x00000013u, 0x00000014u, 0x00000001u, 0x00040020u, 0x00000019u, 0x00000003u,
        0x00000007u, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u,
        0x000200f8u, 0x00000005u, 0x0004003du, 0x00000007u, 0x00000012u, 0x00000011u,
        0x0004003du, 0x0000000eu, 0x00000015u, 0x00000014u, 0x0004006fu, 0x00000006u,
        0x00000016u, 0x00000015u, 0x00070050u, 0x00000007u, 0x00000017u, 0x00000016u,
        0x00000016u, 0x00000016u, 0x00000016u, 0x00050081u, 0x00000007u, 0x00000018u,
        0x00000012u, 0x00000017u, 0x00050041u, 0x00000019u, 0x0000001au, 0x0000000du,
        0x0000000fu, 0x0003003eu, 0x0000001au, 0x00000018u, 0x000100fdu, 0x00010038u,
    };


    // Same, but reading gl_VertexIndex instead: a DIFFERENT input builtin. glslang emits
    // this for GL's gl_VertexID, so nearly every real vertex shader has one - it is what
    // separates "declares some builtin" from "declares the InstanceIndex builtin".
    constexpr Uint32 kVertexIndexVertexSpirv[] = {
        0x07230203u, 0x00010000u, 0x0008000bu, 0x0000001bu, 0x00000000u, 0x00020011u,
        0x00000001u, 0x0006000bu, 0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu,
        0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u, 0x0008000fu, 0x00000000u,
        0x00000004u, 0x6e69616du, 0x00000000u, 0x0000000du, 0x00000011u, 0x00000014u,
        0x00030003u, 0x00000002u, 0x000001c2u, 0x00040005u, 0x00000004u, 0x6e69616du,
        0x00000000u, 0x00060005u, 0x0000000bu, 0x505f6c67u, 0x65567265u, 0x78657472u,
        0x00000000u, 0x00060006u, 0x0000000bu, 0x00000000u, 0x505f6c67u, 0x7469736fu,
        0x006e6f69u, 0x00070006u, 0x0000000bu, 0x00000001u, 0x505f6c67u, 0x746e696fu,
        0x657a6953u, 0x00000000u, 0x00070006u, 0x0000000bu, 0x00000002u, 0x435f6c67u,
        0x4470696cu, 0x61747369u, 0x0065636eu, 0x00070006u, 0x0000000bu, 0x00000003u,
        0x435f6c67u, 0x446c6c75u, 0x61747369u, 0x0065636eu, 0x00030005u, 0x0000000du,
        0x00000000u, 0x00040005u, 0x00000011u, 0x6f506e69u, 0x00000073u, 0x00060005u,
        0x00000014u, 0x565f6c67u, 0x65747265u, 0x646e4978u, 0x00007865u, 0x00030047u,
        0x0000000bu, 0x00000002u, 0x00050048u, 0x0000000bu, 0x00000000u, 0x0000000bu,
        0x00000000u, 0x00050048u, 0x0000000bu, 0x00000001u, 0x0000000bu, 0x00000001u,
        0x00050048u, 0x0000000bu, 0x00000002u, 0x0000000bu, 0x00000003u, 0x00050048u,
        0x0000000bu, 0x00000003u, 0x0000000bu, 0x00000004u, 0x00040047u, 0x00000011u,
        0x0000001eu, 0x00000000u, 0x00040047u, 0x00000014u, 0x0000000bu, 0x0000002au,
        0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u, 0x00030016u,
        0x00000006u, 0x00000020u, 0x00040017u, 0x00000007u, 0x00000006u, 0x00000004u,
        0x00040015u, 0x00000008u, 0x00000020u, 0x00000000u, 0x0004002bu, 0x00000008u,
        0x00000009u, 0x00000001u, 0x0004001cu, 0x0000000au, 0x00000006u, 0x00000009u,
        0x0006001eu, 0x0000000bu, 0x00000007u, 0x00000006u, 0x0000000au, 0x0000000au,
        0x00040020u, 0x0000000cu, 0x00000003u, 0x0000000bu, 0x0004003bu, 0x0000000cu,
        0x0000000du, 0x00000003u, 0x00040015u, 0x0000000eu, 0x00000020u, 0x00000001u,
        0x0004002bu, 0x0000000eu, 0x0000000fu, 0x00000000u, 0x00040020u, 0x00000010u,
        0x00000001u, 0x00000007u, 0x0004003bu, 0x00000010u, 0x00000011u, 0x00000001u,
        0x00040020u, 0x00000013u, 0x00000001u, 0x0000000eu, 0x0004003bu, 0x00000013u,
        0x00000014u, 0x00000001u, 0x00040020u, 0x00000019u, 0x00000003u, 0x00000007u,
        0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u, 0x000200f8u,
        0x00000005u, 0x0004003du, 0x00000007u, 0x00000012u, 0x00000011u, 0x0004003du,
        0x0000000eu, 0x00000015u, 0x00000014u, 0x0004006fu, 0x00000006u, 0x00000016u,
        0x00000015u, 0x00070050u, 0x00000007u, 0x00000017u, 0x00000016u, 0x00000016u,
        0x00000016u, 0x00000016u, 0x00050081u, 0x00000007u, 0x00000018u, 0x00000012u,
        0x00000017u, 0x00050041u, 0x00000019u, 0x0000001au, 0x0000000du, 0x0000000fu,
        0x0003003eu, 0x0000001au, 0x00000018u, 0x000100fdu, 0x00010038u,
    };

    // Owns the reflection module so each test case cleans up after itself.
    class ReflectModule {
    public:
        template <SizeT WordCount>
        explicit ReflectModule(const Uint32 (&spirv)[WordCount]) {
            m_created = spvReflectCreateShaderModule(sizeof(spirv), spirv, &m_module) ==
                        SPV_REFLECT_RESULT_SUCCESS;
        }
        ~ReflectModule() {
            if (m_created) {
                spvReflectDestroyShaderModule(&m_module);
            }
        }
        ReflectModule(const ReflectModule&) = delete;
        ReflectModule& operator=(const ReflectModule&) = delete;

        Bool Created() const { return m_created; }
        const SpvReflectShaderModule& Get() const { return m_module; }

    private:
        SpvReflectShaderModule m_module{};
        Bool m_created = false;
    };

    PipelineFactory::PipelineCreatePayload MakeDepthWritingPayload(
        const VkPipelineColorBlendAttachmentState& attachment0) {
        PipelineFactory::PipelineCreatePayload payload{};
        payload.colorAttachmentCount = 1;
        payload.depthTestEnable = true;
        payload.depthWriteEnable = true;
        payload.colorBlendAttachments[0] = attachment0;
        return payload;
    }
} // namespace

// --- Device gate: MOBILEGL_MAGMA_DISABLE_BLENDED_DEPTH_WRITE tri-state ---

TEST(PipelineQuirkDeviceGate, ForceOnEnablesOnAnyVendor) {
    EXPECT_TRUE(PipelineFactory::ShouldSuppressBlendedDepthWriteForDevice(QuirkOverride::ForceOn,
                                                                          kVendorIdArm));
    EXPECT_TRUE(PipelineFactory::ShouldSuppressBlendedDepthWriteForDevice(QuirkOverride::ForceOn,
                                                                          kVendorIdQualcomm));
}

TEST(PipelineQuirkDeviceGate, ForceOffDisablesEvenOnQualcomm) {
    EXPECT_FALSE(PipelineFactory::ShouldSuppressBlendedDepthWriteForDevice(QuirkOverride::ForceOff,
                                                                           kVendorIdQualcomm));
}

TEST(PipelineQuirkDeviceGate, AutoDetectsQualcommOnly) {
    EXPECT_TRUE(PipelineFactory::ShouldSuppressBlendedDepthWriteForDevice(QuirkOverride::Auto,
                                                                          kVendorIdQualcomm));
    EXPECT_FALSE(PipelineFactory::ShouldSuppressBlendedDepthWriteForDevice(QuirkOverride::Auto,
                                                                           kVendorIdArm));
}

TEST(PipelineQuirkDeviceGate, ForceOnRoundTripsThroughTheFactoryFlag) {
    const Bool previous = PipelineFactory::IsSuppressBlendedDepthWriteEnabled();
    PipelineFactory::SetSuppressBlendedDepthWrite(
        PipelineFactory::ShouldSuppressBlendedDepthWriteForDevice(QuirkOverride::ForceOn, kVendorIdArm));
    EXPECT_TRUE(PipelineFactory::IsSuppressBlendedDepthWriteEnabled());
    PipelineFactory::SetSuppressBlendedDepthWrite(previous);
}

// --- Per-pipeline strip decision against the pipeline create-info payload ---

TEST(PipelineQuirkStripDecision, MaxBlendIsStripped) {
    // MC 26.3 OIT depth_bounds: GL_MAX accumulation writing depth - the case the quirk fixes.
    const auto payload = MakeDepthWritingPayload(MakeBlendAttachment(
        true, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_MAX, kFullColorWriteMask));
    EXPECT_TRUE(PipelineFactory::ShouldSuppressDepthWrite(payload));
}

TEST(PipelineQuirkStripDecision, MinBlendIsStripped) {
    const auto payload = MakeDepthWritingPayload(MakeBlendAttachment(
        true, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_MIN, kFullColorWriteMask));
    EXPECT_TRUE(PipelineFactory::ShouldSuppressDepthWrite(payload));
}

TEST(PipelineQuirkStripDecision, AdditiveOnePlusOneIsNotStripped) {
    // ONE+ONE additive with a depth write matched zero draws of the 26.3 chain in the
    // fixture sweep (transmittance/accumulate disable depth writes themselves); the only
    // real content with this shape was harmless additive glow effects (Create). A quirk
    // touches as little unrelated content as possible, so the shape stays exempt.
    const auto payload = MakeDepthWritingPayload(MakeBlendAttachment(
        true, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD, kFullColorWriteMask));
    EXPECT_FALSE(PipelineFactory::ShouldSuppressDepthWrite(payload));
}

TEST(PipelineQuirkStripDecision, SortedTransparencyOverBlendIsNotStripped) {
    // Vanilla MC translucent layer (water, stained glass): SRC_ALPHA "over" compositing
    // draws each surface once and depends on its depth writes to occlude particles, rain,
    // and clouds drawn later - it must keep them.
    const auto payload = MakeDepthWritingPayload(MakeBlendAttachment(
        true, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD,
        kFullColorWriteMask));
    EXPECT_FALSE(PipelineFactory::ShouldSuppressDepthWrite(payload));
}

TEST(PipelineQuirkStripDecision, EffectivelyOpaqueBlendIsNotStripped) {
    // GL_BLEND left enabled with ONE/ZERO+ADD factors is opaque in effect; stripping its
    // depth write would break occlusion for plainly opaque geometry.
    const auto payload = MakeDepthWritingPayload(MakeBlendAttachment(
        true, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, kFullColorWriteMask));
    EXPECT_FALSE(PipelineFactory::ShouldSuppressDepthWrite(payload));
}

TEST(PipelineQuirkStripDecision, FullyMaskedAccumulationBlendIsNotStripped) {
    // Depth-prepass pattern: colorMask(0,0,0,0) with blending left enabled - blending is
    // moot, and stripping would delete the entire prepass. MAX so the exemption, not the
    // blend-op filter, is what keeps the depth write.
    const auto payload = MakeDepthWritingPayload(MakeBlendAttachment(
        true, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_MAX, 0));
    EXPECT_FALSE(PipelineFactory::ShouldSuppressDepthWrite(payload));
}

TEST(PipelineQuirkStripDecision, DisabledBlendIsNotStripped) {
    const auto payload = MakeDepthWritingPayload(MakeBlendAttachment(
        false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_MAX, kFullColorWriteMask));
    EXPECT_FALSE(PipelineFactory::ShouldSuppressDepthWrite(payload));
}

TEST(PipelineQuirkStripDecision, NoDepthWriteMeansNoStrip) {
    auto payload = MakeDepthWritingPayload(MakeBlendAttachment(
        true, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_MAX, kFullColorWriteMask));
    payload.depthWriteEnable = false;
    EXPECT_FALSE(PipelineFactory::ShouldSuppressDepthWrite(payload));
}

TEST(PipelineQuirkStripDecision, FragDepthWriterIsExempt) {
    // gl_FragDepth output does not go through per-pipeline vertex position math, so the
    // cross-pipeline invariance hazard cannot affect it (e.g. the 26.3 OIT composite).
    auto payload = MakeDepthWritingPayload(MakeBlendAttachment(
        true, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_MAX, kFullColorWriteMask));
    payload.fragmentReplacesDepth = true;
    EXPECT_FALSE(PipelineFactory::ShouldSuppressDepthWrite(payload));
}

TEST(PipelineQuirkStripDecision, AccumulationOnSecondaryAttachmentIsStripped) {
    // The scan is not limited to attachment 0: an extremum accumulation on any live
    // attachment marks the pipeline.
    PipelineFactory::PipelineCreatePayload payload{};
    payload.colorAttachmentCount = 2;
    payload.depthTestEnable = true;
    payload.depthWriteEnable = true;
    payload.colorBlendAttachments[0] = MakeBlendAttachment(
        false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, kFullColorWriteMask);
    payload.colorBlendAttachments[1] = MakeBlendAttachment(
        true, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_MAX, kFullColorWriteMask);
    EXPECT_TRUE(PipelineFactory::ShouldSuppressDepthWrite(payload));
}

TEST(PipelineQuirkStripDecision, AlphaWeightedAdditiveIsNotStripped) {
    // SRC_ALPHA,ONE additive: the classic *sorted* particle/glow blend. Kept exempt like
    // every other ADD-op shape now that the strip is extremum-only.
    const auto payload = MakeDepthWritingPayload(MakeBlendAttachment(
        true, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD, kFullColorWriteMask));
    EXPECT_FALSE(PipelineFactory::ShouldSuppressDepthWrite(payload));
}

TEST(PipelineQuirkStripDecision, ReverseSubtractIsNotStripped) {
    // Deliberate narrowing: only the MIN/MAX extremum ops carry the depth-bounds
    // signature. SUBTRACT-class ops stay outside the quirk until content demands them.
    const auto payload = MakeDepthWritingPayload(MakeBlendAttachment(
        true, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_REVERSE_SUBTRACT,
        kFullColorWriteMask));
    EXPECT_FALSE(PipelineFactory::ShouldSuppressDepthWrite(payload));
}

TEST(PipelineQuirkStripDecision, PartiallyMaskedAccumulationIsStripped) {
    // Only a fully masked attachment is exempt; a live alpha channel still accumulates.
    const auto payload = MakeDepthWritingPayload(MakeBlendAttachment(
        true, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_MAX, VK_COLOR_COMPONENT_A_BIT));
    EXPECT_TRUE(PipelineFactory::ShouldSuppressDepthWrite(payload));
}

TEST(PipelineQuirkStripDecision, NoColorAttachmentsMeansNoStrip) {
    // Depth-only FBO: the loop must not read the (stale) attachment array at all.
    PipelineFactory::PipelineCreatePayload payload{};
    payload.colorAttachmentCount = 0;
    payload.depthTestEnable = true;
    payload.depthWriteEnable = true;
    payload.colorBlendAttachments[0] = MakeBlendAttachment(
        true, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_MAX, kFullColorWriteMask);
    EXPECT_FALSE(PipelineFactory::ShouldSuppressDepthWrite(payload));
}

TEST(PipelineQuirkStripDecision, SeparateAlphaAccumulationIsNotStripped) {
    // glBlendEquationSeparate(GL_FUNC_ADD, GL_MAX) over an ordinary color over-blend: the
    // alpha channel accumulates but the color channel does not. Pins that the decision is
    // color-channel only - widening it to alpha would re-capture sorted transparency.
    auto attachment = MakeBlendAttachment(true, VK_BLEND_FACTOR_SRC_ALPHA,
                                          VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD,
                                          kFullColorWriteMask);
    attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    attachment.alphaBlendOp = VK_BLEND_OP_MAX;
    EXPECT_FALSE(PipelineFactory::ShouldSuppressDepthWrite(MakeDepthWritingPayload(attachment)));
}

TEST(PipelineQuirkStripDecision, MixedOverAndMaskedAttachmentsAreNotStripped) {
    PipelineFactory::PipelineCreatePayload payload{};
    payload.colorAttachmentCount = 2;
    payload.depthTestEnable = true;
    payload.depthWriteEnable = true;
    payload.colorBlendAttachments[0] = MakeBlendAttachment(
        true, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD,
        kFullColorWriteMask);
    payload.colorBlendAttachments[1] = MakeBlendAttachment(
        true, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_MAX, 0);
    EXPECT_FALSE(PipelineFactory::ShouldSuppressDepthWrite(payload));
}

// --- DepthReplacing reflection feeding the gl_FragDepth exemption ---

TEST(ReflectedFragmentReplacesDepth, TrueForAShaderThatAssignsFragDepth) {
    const ReflectModule module(kFragDepthWriterSpirv);
    ASSERT_TRUE(module.Created());
    EXPECT_TRUE(ProgramFactory::ReflectedFragmentReplacesDepth(module.Get()));
}

TEST(ReflectedFragmentReplacesDepth, FalseForAPlainFragmentShader) {
    const ReflectModule module(kPlainFragmentSpirv);
    ASSERT_TRUE(module.Created());
    EXPECT_FALSE(ProgramFactory::ReflectedFragmentReplacesDepth(module.Get()));
}

TEST(ReflectedFragmentReplacesDepth, FalseForAnEmptyModule) {
    // A default-constructed module has no entry points; the scan must not dereference.
    SpvReflectShaderModule emptyModule{};
    EXPECT_FALSE(ProgramFactory::ReflectedFragmentReplacesDepth(emptyModule));
}

TEST(ReflectedFragmentReplacesDepth, ReflectedFlagFlipsTheStripDecision) {
    // The two fixtures differ only by the gl_FragDepth assignment, so they pin that the
    // reflected flag is what flips the strip decision for an otherwise identical pipeline.
    const ReflectModule depthWriter(kFragDepthWriterSpirv);
    const ReflectModule plain(kPlainFragmentSpirv);
    ASSERT_TRUE(depthWriter.Created());
    ASSERT_TRUE(plain.Created());

    auto payload = MakeDepthWritingPayload(MakeBlendAttachment(
        true, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_MAX, kFullColorWriteMask));

    payload.fragmentReplacesDepth = ProgramFactory::ReflectedFragmentReplacesDepth(plain.Get());
    EXPECT_TRUE(PipelineFactory::ShouldSuppressDepthWrite(payload));

    payload.fragmentReplacesDepth = ProgramFactory::ReflectedFragmentReplacesDepth(depthWriter.Get());
    EXPECT_FALSE(PipelineFactory::ShouldSuppressDepthWrite(payload));
}


// --- InstanceIndex reflection feeding the shaderDrawParameters diagnostic ---

TEST(ReflectedReadsInstanceIndexBuiltin, TrueForAShaderReadingInstanceIndex) {
    const ReflectModule module(kInstanceIndexVertexSpirv);
    ASSERT_TRUE(module.Created());
    EXPECT_TRUE(ProgramFactory::ReflectedReadsInstanceIndexBuiltin(module.Get()));
}

TEST(ReflectedReadsInstanceIndexBuiltin, FalseForAShaderReadingADifferentBuiltin) {
    // Discriminates the builtin's identity, not merely its presence: weakening the check to
    // "has any BuiltIn decoration" would fire the diagnostic on every real vertex shader.
    const ReflectModule module(kVertexIndexVertexSpirv);
    ASSERT_TRUE(module.Created());
    EXPECT_FALSE(ProgramFactory::ReflectedReadsInstanceIndexBuiltin(module.Get()));
}

TEST(ReflectedReadsInstanceIndexBuiltin, FalseForAShaderWithNoInputBuiltins) {
    const ReflectModule module(kPlainFragmentSpirv);
    ASSERT_TRUE(module.Created());
    EXPECT_FALSE(ProgramFactory::ReflectedReadsInstanceIndexBuiltin(module.Get()));
}

TEST(ReflectedReadsInstanceIndexBuiltin, FalseForAnEmptyModule) {
    SpvReflectShaderModule emptyModule{};
    EXPECT_FALSE(ProgramFactory::ReflectedReadsInstanceIndexBuiltin(emptyModule));
}


TEST(WirePipelineCompatibility, ExactCompatibilitySurvivesNativeObjectTurnover) {
#if MOBILEGL_BUILD_DISAGGREGATED
    using namespace MobileGL::MG_Backend::DirectVulkan;
    WireRenderPassCompatibilityTable table;
    WireRenderPassCompatibilityKey a{{(Uint64(VK_FORMAT_R8G8B8A8_UNORM) << 32) | VK_SAMPLE_COUNT_1_BIT}, {0},
                                      VK_ATTACHMENT_UNUSED};
    const Uint64 first = table.Intern(a);
    ASSERT_NE(first, 0u);
    auto b = a;
    b.attachmentFormatsAndSamples[0] = (Uint64(VK_FORMAT_R8G8B8A8_SRGB) << 32) | VK_SAMPLE_COUNT_1_BIT;
    EXPECT_NE(table.Intern(b), first);
    b = a;
    b.attachmentFormatsAndSamples[0] = (Uint64(VK_FORMAT_R8G8B8A8_UNORM) << 32) | VK_SAMPLE_COUNT_4_BIT;
    EXPECT_NE(table.Intern(b), first);
    b = a;
    b.colorReferences = {VK_ATTACHMENT_UNUSED, 0};
    EXPECT_NE(table.Intern(b), first);
    b = a;
    b.depthReference = 0;
    EXPECT_NE(table.Intern(b), first);
    // Native pass/image turnover and swapchain changes never reset this table.
    EXPECT_EQ(table.Intern(a), first);
#else
    GTEST_SKIP() << "wire pipeline compatibility requires disaggregated build";
#endif
}

TEST(WirePipelineCompatibility, HashUsesCompatibilityIdentityAndSeparatesNativeDomain) {
#if MOBILEGL_BUILD_DISAGGREGATED
    using namespace MobileGL::MG_Backend::DirectVulkan;
    VulkanRendererConfig config{};
    config.DisablePipelineCache = true;
    // Hash-only unit: DisablePipelineCache prevents all device calls, and no
    // pipeline is created. These tokens are never passed to a Vulkan driver.
    PipelineFactory factory(reinterpret_cast<VkDevice>(uintptr_t{1}), config);
    PipelineFactory::PipelineCreatePayload payload{};
    payload.renderPass = (VkRenderPass)(uintptr_t{1});
    payload.wireRenderPassCompatibilityId = 1;
    const auto a = factory.ComputeHash(payload);
    payload.renderPass = (VkRenderPass)(uintptr_t{2});
    EXPECT_EQ(factory.ComputeHash(payload), a) << "compatible passes must reuse the pipeline across native handles";
    payload.wireRenderPassCompatibilityId = 2;
    EXPECT_NE(factory.ComputeHash(payload), a) << "a recycled native handle must not alias incompatible passes";
    payload.renderPass = (VkRenderPass)(uintptr_t{1});
    payload.wireRenderPassCompatibilityId = 0;
    EXPECT_NE(factory.ComputeHash(payload), a) << "native handles and wire identities are different domains";
    const auto native = factory.ComputeHash(payload);
    payload.renderPass = (VkRenderPass)(uintptr_t{2});
    EXPECT_NE(factory.ComputeHash(payload), native);
#else
    GTEST_SKIP() << "wire pipeline compatibility requires disaggregated build";
#endif
}

// P7 gate 5 (g5-msrbo, g5-msprobe): the multisample depth/stencil resolve's arm order is the
// resolve probe's VERDICT (WireDepthResolveArm.h), not the device's vendor. On the Redmi (Adreno
// 830) the no-draw VK_KHR_depth_stencil_resolve render pass left its target unwritten while the
// shader arm resolved correctly; the probe reproduces that on the actual device with the shader
// arm as its control. These tests pin every mapping from a measurement to the order through a
// FAKE RESULT SOURCE - a canned measurement handed to ChooseWireDepthResolveArm in place of the
// Vulkan probe.
#if MOBILEGL_BUILD_DISAGGREGATED
namespace {
    using MobileGL::MG_Backend::DirectVulkan::ChooseWireDepthResolveArm;
    using MobileGL::MG_Backend::DirectVulkan::DescribeWireDepthResolveProbe;
    using MobileGL::MG_Backend::DirectVulkan::EvaluateWireDepthResolveProbe;
    using MobileGL::MG_Backend::DirectVulkan::ParseWireDepthResolveProbeKnob;
    using MobileGL::MG_Backend::DirectVulkan::WireDepthResolveAspectReading;
    using MobileGL::MG_Backend::DirectVulkan::WireDepthResolveFormatReading;
    using MobileGL::MG_Backend::DirectVulkan::WireDepthResolveProbeKnob;
    using MobileGL::MG_Backend::DirectVulkan::WireDepthResolveProbeMeasurement;
    using MobileGL::MG_Backend::DirectVulkan::WireDepthResolveProbeVerdict;

    // How one aspect's two arms read back, out of 16 texels.
    struct FakeArms {
        Uint32 renderPassMatches = 16;
        Bool shaderRan = true;
        Uint32 shaderMatches = 16;
    };

    WireDepthResolveAspectReading FakeAspect(Uint32 expected, Uint32 sentinel, FakeArms arms) {
        WireDepthResolveAspectReading aspect;
        aspect.measured = true;
        aspect.texels = 16;
        aspect.expected = expected;
        aspect.sentinel = sentinel;
        aspect.renderPassMatches = arms.renderPassMatches;
        aspect.renderPassSentinels = 16 - arms.renderPassMatches;
        aspect.renderPassFirst = arms.renderPassMatches == 16 ? expected : sentinel;
        aspect.shaderRan = arms.shaderRan;
        aspect.shaderMatches = arms.shaderRan ? arms.shaderMatches : 0;
        aspect.shaderFirst = arms.shaderMatches == 16 ? expected : 0;
        return aspect;
    }

    // A packed format (both aspects) or a depth-only one (stencil = nullptr-like: not measured).
    WireDepthResolveFormatReading FakeFormat(const char* name, FakeArms depth, Optional<FakeArms> stencil) {
        WireDepthResolveFormatReading reading;
        reading.name = name;
        reading.samples = 4;
        reading.ran = true;
        reading.depth = FakeAspect(0x400000u, 0xBFFFFFu, depth);
        if (stencil) reading.stencil = FakeAspect(0x5Au, 0xA5u, *stencil);
        return reading;
    }

    WireDepthResolveProbeMeasurement FakeMeasurement(Vector<WireDepthResolveFormatReading> formats) {
        WireDepthResolveProbeMeasurement measurement;
        measurement.ran = true;
        measurement.formats = Move(formats);
        return measurement;
    }

    // The Adreno 830's reading: the render pass left the sentinel in every texel of both aspects,
    // the shader control resolved all of them.
    constexpr FakeArms kRenderPassWroteNothing{0, true, 16};
    constexpr FakeArms kBothArmsResolve{16, true, 16};
    // Neither arm resolved: the probe measured its own setup, or a device that cannot resolve.
    constexpr FakeArms kControlFailsToo{0, true, 0};
    constexpr FakeArms kNoStencilExport{0, false, 0};
} // namespace
#endif

TEST(PipelineQuirkTest, WireDepthResolveProbeDetectsARenderPassThatWritesNothing) {
#if MOBILEGL_BUILD_DISAGGREGATED
    Int measured = 0;
    const auto choice = ChooseWireDepthResolveArm(WireDepthResolveProbeKnob::Measure, true, [&] {
        ++measured;
        return FakeMeasurement({FakeFormat("D24_UNORM_S8_UINT", kRenderPassWroteNothing, kRenderPassWroteNothing),
                                FakeFormat("D32_SFLOAT", kRenderPassWroteNothing, std::nullopt)});
    });
    EXPECT_EQ(measured, 1);
    EXPECT_EQ(choice.verdict, WireDepthResolveProbeVerdict::RenderPassResolveBroken);
    EXPECT_TRUE(choice.preferShader) << "the render pass writes nothing and the shader control resolves: the "
                                        "shader arm must go first";
    // Writing WRONG values is the same defect as writing none.
    auto wrong = FakeMeasurement({FakeFormat("D16_UNORM", kBothArmsResolve, std::nullopt)});
    wrong.formats[0].depth.renderPassMatches = 15;
    wrong.formats[0].depth.renderPassSentinels = 0;
    EXPECT_EQ(EvaluateWireDepthResolveProbe(wrong), WireDepthResolveProbeVerdict::RenderPassResolveBroken);
    // One aspect of one format is enough: the render pass resolves every aspect in one pass.
    EXPECT_EQ(EvaluateWireDepthResolveProbe(FakeMeasurement(
                  {FakeFormat("D24_UNORM_S8_UINT", kBothArmsResolve, kRenderPassWroteNothing),
                   FakeFormat("D32_SFLOAT", kBothArmsResolve, std::nullopt)})),
              WireDepthResolveProbeVerdict::RenderPassResolveBroken);
    EXPECT_NE(DescribeWireDepthResolveProbe(choice.measurement).find("render pass 0/16"), String::npos);
#else
    GTEST_SKIP() << "the wire resolve arms exist only in the disaggregated build";
#endif
}

TEST(PipelineQuirkTest, WireDepthResolveProbeKeepsTheRenderPassFirstWhereItResolves) {
#if MOBILEGL_BUILD_DISAGGREGATED
    const auto choice = ChooseWireDepthResolveArm(WireDepthResolveProbeKnob::Measure, true, [] {
        return FakeMeasurement({FakeFormat("D24_UNORM_S8_UINT", kBothArmsResolve, kBothArmsResolve),
                                FakeFormat("D32_SFLOAT", kBothArmsResolve, std::nullopt)});
    });
    EXPECT_EQ(choice.verdict, WireDepthResolveProbeVerdict::Clean);
    EXPECT_FALSE(choice.preferShader) << "lavapipe's reading: the fast arm keeps its place";
#else
    GTEST_SKIP() << "the wire resolve arms exist only in the disaggregated build";
#endif
}

// THE CONTROL (DriverBugProbes.h's iron rule): a render pass that failed where the shader pass
// failed too is not evidence about the render pass.
TEST(PipelineQuirkTest, WireDepthResolveProbeIsInconclusiveWhenTheShaderControlFailsToo) {
#if MOBILEGL_BUILD_DISAGGREGATED
    const auto choice = ChooseWireDepthResolveArm(WireDepthResolveProbeKnob::Measure, true, [] {
        return FakeMeasurement({FakeFormat("D24_UNORM_S8_UINT", kControlFailsToo, kControlFailsToo),
                                FakeFormat("D32_SFLOAT", kControlFailsToo, std::nullopt)});
    });
    EXPECT_EQ(choice.verdict, WireDepthResolveProbeVerdict::Inconclusive);
    EXPECT_FALSE(choice.preferShader) << "an inconclusive probe must never move the arm order";
    // No control at all (no shader kit) is the same answer.
    auto uncontrolled = FakeMeasurement({FakeFormat("D32_SFLOAT", kNoStencilExport, std::nullopt)});
    EXPECT_EQ(EvaluateWireDepthResolveProbe(uncontrolled), WireDepthResolveProbeVerdict::Inconclusive);
    // A stencil aspect with no control (no VK_EXT_shader_stencil_export) is not counted, either
    // way: its render-pass failure proves nothing, and the controlled depth aspect decides.
    EXPECT_EQ(EvaluateWireDepthResolveProbe(
                  FakeMeasurement({FakeFormat("D24_UNORM_S8_UINT", kBothArmsResolve, kNoStencilExport)})),
              WireDepthResolveProbeVerdict::Clean);
    // A format whose control failed does not veto another format's conclusive defect.
    EXPECT_EQ(EvaluateWireDepthResolveProbe(
                  FakeMeasurement({FakeFormat("D16_UNORM", kControlFailsToo, std::nullopt),
                                   FakeFormat("D32_SFLOAT", kRenderPassWroteNothing, std::nullopt)})),
              WireDepthResolveProbeVerdict::RenderPassResolveBroken);
    // Skipped formats and a probe that did not run (or timed out) conclude nothing.
    auto skipped = FakeMeasurement({FakeFormat("X8_D24_UNORM_PACK32", kRenderPassWroteNothing, std::nullopt)});
    skipped.formats[0].ran = false;
    EXPECT_EQ(EvaluateWireDepthResolveProbe(skipped), WireDepthResolveProbeVerdict::Inconclusive);
    auto timedOut = FakeMeasurement({FakeFormat("D32_SFLOAT", kRenderPassWroteNothing, std::nullopt)});
    timedOut.fenceWaitTimedOut = true;
    EXPECT_EQ(EvaluateWireDepthResolveProbe(timedOut), WireDepthResolveProbeVerdict::NotRun);
    EXPECT_EQ(EvaluateWireDepthResolveProbe(WireDepthResolveProbeMeasurement{}), WireDepthResolveProbeVerdict::NotRun);
#else
    GTEST_SKIP() << "the wire resolve arms exist only in the disaggregated build";
#endif
}

TEST(PipelineQuirkTest, WireDepthResolveProbeRunsOnlyWhereThereIsARenderPassArmToJudge) {
#if MOBILEGL_BUILD_DISAGGREGATED
    Int measured = 0;
    const auto choice = ChooseWireDepthResolveArm(WireDepthResolveProbeKnob::Measure, false, [&] {
        ++measured;
        return FakeMeasurement({FakeFormat("D32_SFLOAT", kRenderPassWroteNothing, std::nullopt)});
    });
    EXPECT_EQ(measured, 0) << "without VK_KHR_depth_stencil_resolve the shader pass is the only arm";
    EXPECT_EQ(choice.verdict, WireDepthResolveProbeVerdict::NotRun);
    EXPECT_FALSE(choice.preferShader);
#else
    GTEST_SKIP() << "the wire resolve arms exist only in the disaggregated build";
#endif
}

// MGITEST_MAGMA_DEPTH_RESOLVE_PROBE replaces the MEASUREMENT, not the verdict: the canned one goes
// through the same evaluation, which is what lets the MsResolveBug./MsFlipBug. entries red when
// the evaluation stops detecting the defect.
TEST(PipelineQuirkTest, WireDepthResolveProbeKnobReplacesTheMeasurementNotTheVerdict) {
#if MOBILEGL_BUILD_DISAGGREGATED
    EXPECT_EQ(ParseWireDepthResolveProbeKnob(nullptr), WireDepthResolveProbeKnob::Measure);
    EXPECT_EQ(ParseWireDepthResolveProbeKnob(""), WireDepthResolveProbeKnob::Measure);
    EXPECT_EQ(ParseWireDepthResolveProbeKnob("bug"), WireDepthResolveProbeKnob::ForceBug);
    EXPECT_EQ(ParseWireDepthResolveProbeKnob("clean"), WireDepthResolveProbeKnob::ForceClean);
    EXPECT_EQ(ParseWireDepthResolveProbeKnob("0x5143"), WireDepthResolveProbeKnob::Unrecognised);
    Int measured = 0;
    const auto measure = [&] {
        ++measured;
        return FakeMeasurement({FakeFormat("D32_SFLOAT", kBothArmsResolve, std::nullopt)});
    };
    const auto bug = ChooseWireDepthResolveArm(WireDepthResolveProbeKnob::ForceBug, true, measure);
    EXPECT_TRUE(bug.measurement.fromKnob);
    EXPECT_EQ(bug.verdict, WireDepthResolveProbeVerdict::RenderPassResolveBroken);
    EXPECT_TRUE(bug.preferShader);
    const auto clean = ChooseWireDepthResolveArm(WireDepthResolveProbeKnob::ForceClean, true, measure);
    EXPECT_TRUE(clean.measurement.fromKnob);
    EXPECT_EQ(clean.verdict, WireDepthResolveProbeVerdict::Clean);
    EXPECT_FALSE(clean.preferShader);
    EXPECT_EQ(measured, 0) << "a forced verdict must not also run the device probe";
    const auto other = ChooseWireDepthResolveArm(WireDepthResolveProbeKnob::Unrecognised, true, measure);
    EXPECT_EQ(measured, 1) << "an unrecognised value runs the probe as if the knob were unset";
    EXPECT_FALSE(other.measurement.fromKnob);
    EXPECT_EQ(other.verdict, WireDepthResolveProbeVerdict::Clean);
#else
    GTEST_SKIP() << "the wire resolve arms exist only in the disaggregated build";
#endif
}
