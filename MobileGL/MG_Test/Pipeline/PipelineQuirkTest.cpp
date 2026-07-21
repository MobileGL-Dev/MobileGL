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
