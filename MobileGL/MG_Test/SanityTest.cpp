// MobileGL - MobileGL/MG_Test/SanityTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <gtest/gtest.h>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <MG_Backend/DirectGLES/BackendObject_DirectGLES.h>
#include <MG_Backend/DirectGLES/Managers.h>
#include <MG_Backend/DirectVulkan/BackendObject_DirectVulkan.h>
#include <MG_Backend/BackendObjects.h>
#include <MG_Impl/GLImpl/Getter/GL_Getter.h>
#include <MG_Impl/GLImpl/RenderState/GL_RenderState.h>
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/FramebufferState/FramebufferObject.h>
#include <MG_State/GLState/TextureState/TextureState.h>
#include <MG_Backend/DirectVulkan/Renderer/VkRenderPassManager.h>
#include <MG_Backend/DirectVulkan/Renderer/VkTextureManager.h>
#include <MG_Backend/DirectVulkan/Renderer/VulkanRenderer.h>
#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>
#include <MG_Util/ShaderTranspiler/ShaderSourceProcessor.h>
#include <MG_Util/Debug/Log.h>

namespace {
    class DynamicParameterBackend final : public MobileGL::MG_Backend::BackendObject {
    public:
        explicit DynamicParameterBackend(MobileGL::MG_Backend::DynamicBackendParameters params):
            m_params(params) {}

        void Initialize() override {}
        MobileGL::Bool InitCapabilities() override { return true; }
        MobileGL::Bool InitWindowSurface() override { return true; }
        const MobileGL::RendererInfo& GetRendererInfo() const override { return m_info; }
        MobileGL::String GetBackendAPIVersionString() const override { return "test"; }
        const MobileGL::MG_Backend::GlobalBackendFunctionsTable& GetBackendFunctions() const override {
            return m_functions;
        }
        const MobileGL::MG_Backend::DynamicBackendParameters& GetDynamicParameters() const override {
            return m_params;
        }
        MobileGL::BackendType GetBackendType() const override { return MobileGL::BackendType::Unknown; }

    private:
        MobileGL::MG_Backend::DynamicBackendParameters m_params;
        MobileGL::MG_Backend::GlobalBackendFunctionsTable m_functions{};
        MobileGL::RendererInfo m_info{
            .RendererName = "Test",
            .BackendName = "DynamicParameterBackend",
            .ExtraVendor = MobileGL::Nullopt,
            .RendererGLInfo = {.TargetGLVersion = {3, 3, 0},
                               .TargetGLSLVersion = {4, 6, 0},
                               .Extensions = {},
                               .IsCompatibilityProfile = false},
            .StaticBackendCapability = {.AllowVSOnlyPrograms = false}};
    };

    void SetEnvVar(const char* name, const char* value) {
#if defined(_WIN32)
        _putenv_s(name, value);
#else
        setenv(name, value, 1);
#endif
    }

    void UnsetEnvVar(const char* name) {
#if defined(_WIN32)
        _putenv_s(name, "");
#else
        unsetenv(name);
#endif
    }
} // namespace

TEST(Sanity, BasicAssertions) {
    // Expect two strings not to be equal.
    EXPECT_STRNE("hello", "world");
    // Expect equality.
    EXPECT_EQ(7 * 6, 42);
}

TEST(DirectGLESSanity, AdvertisesDepthTextureForGlmarkShadowScenes) {
    MobileGL::MG_Backend::DirectGLES::BackendObject_DirectGLES backend;
    const auto& extensions = backend.GetRendererInfo().RendererGLInfo.Extensions;

    EXPECT_NE(std::find(extensions.begin(), extensions.end(), MobileGL::E_GL_ARB_depth_texture), extensions.end());
}

TEST(DirectGLESSanity, AdvertisesVoxyRequiredRenderingExtensionsWithoutRaisingGLVersion) {
    MobileGL::MG_Backend::DirectGLES::BackendObject_DirectGLES backend;
    const auto& rendererInfo = backend.GetRendererInfo().RendererGLInfo;
    const auto& extensions = rendererInfo.Extensions;

    EXPECT_EQ(rendererInfo.TargetGLVersion.Major, 3);
    EXPECT_EQ(rendererInfo.TargetGLVersion.Minor, 3);
    EXPECT_EQ(rendererInfo.TargetGLVersion.Patch, 0);

    EXPECT_NE(std::find(extensions.begin(), extensions.end(), MobileGL::E_GL_ARB_compute_shader),
              extensions.end());
    EXPECT_NE(std::find(extensions.begin(), extensions.end(), MobileGL::E_GL_ARB_shader_storage_buffer_object),
              extensions.end());
    EXPECT_NE(std::find(extensions.begin(), extensions.end(), MobileGL::E_GL_ARB_texture_storage),
              extensions.end());
    EXPECT_NE(std::find(extensions.begin(), extensions.end(), MobileGL::E_GL_ARB_direct_state_access),
              extensions.end());
    EXPECT_NE(std::find(extensions.begin(), extensions.end(), MobileGL::E_GL_ARB_multi_draw_indirect),
              extensions.end());
    EXPECT_NE(std::find(extensions.begin(), extensions.end(), MobileGL::E_GL_ARB_indirect_parameters),
              extensions.end());
    EXPECT_NE(std::find(extensions.begin(), extensions.end(), MobileGL::E_GL_ARB_shader_draw_parameters),
              extensions.end());
    EXPECT_EQ(std::find(extensions.begin(), extensions.end(), MobileGL::E_GL_ARB_gpu_shader_int64),
              extensions.end());
    EXPECT_EQ(std::find(extensions.begin(), extensions.end(), MobileGL::E_GL_KHR_shader_subgroup),
              extensions.end());
}

TEST(DirectGLESSanity, ProvidesNamedFramebufferBlitForDirectStateAccess) {
    MobileGL::MG_Backend::DirectGLES::BackendObject_DirectGLES backend;
    const auto& funcs = backend.GetBackendFunctions().GL;

    EXPECT_NE(funcs.ClearNamedFramebufferfv, nullptr);
    EXPECT_NE(funcs.ClearNamedFramebufferfi, nullptr);
    EXPECT_NE(funcs.BlitFramebuffer, nullptr);
    EXPECT_NE(funcs.BlitNamedFramebuffer, nullptr);
}

TEST(DirectGLESSanity, RewritesBaseInstanceBuiltinForEsslVertexShaders) {
    const MobileGL::String source = R"(#version 320 es
void main() {
    uint drawId = gl_BaseInstance;
    uint untouched = my_gl_BaseInstance_value;
}
)";

    const auto rewritten = MobileGL::MG_Backend::DirectGLES::EmulateBaseInstanceInVertexShader(
        source, GL_VERTEX_SHADER);

    EXPECT_NE(rewritten.find("uniform highp int mg_BaseInstance;"), MobileGL::String::npos);
    EXPECT_NE(rewritten.find("uint drawId = mg_BaseInstance;"), MobileGL::String::npos);
    EXPECT_NE(rewritten.find("my_gl_BaseInstance_value"), MobileGL::String::npos);
    EXPECT_EQ(rewritten.find("uint drawId = gl_BaseInstance;"), MobileGL::String::npos);
}

TEST(DirectGLESSanity, LeavesBaseInstanceBuiltinAloneOutsideVertexShaders) {
    const MobileGL::String source = "#version 320 es\nuint value = gl_BaseInstance;\n";

    const auto rewritten = MobileGL::MG_Backend::DirectGLES::EmulateBaseInstanceInVertexShader(
        source, GL_FRAGMENT_SHADER);

    EXPECT_EQ(rewritten, source);
}

TEST(DirectVulkanSanity, AdvertisesTextureStorageForDirectStateAccess) {
    MobileGL::MG_Backend::DirectVulkan::BackendObject_DirectVulkan backend;
    const auto& extensions = backend.GetRendererInfo().RendererGLInfo.Extensions;

    EXPECT_NE(std::find(extensions.begin(), extensions.end(), MobileGL::E_GL_ARB_direct_state_access),
              extensions.end());
    EXPECT_NE(std::find(extensions.begin(), extensions.end(), MobileGL::E_GL_ARB_texture_storage), extensions.end());
}

TEST(DirectVulkanSanity, RenderPassExtentUsesSwapchainSizeOnlyForDefaultFramebuffer) {
    using MobileGL::MG_Backend::DirectVulkan::ResolveRenderPassFramebufferExtent;

    const MobileGL::TextureSize attachmentExtent = {512, 512, 1};
    const VkExtent2D swapchainExtent = {3200u, 1440u};

    EXPECT_EQ(ResolveRenderPassFramebufferExtent(true, attachmentExtent, swapchainExtent),
              MobileGL::IntVec2(3200, 1440));
    EXPECT_EQ(ResolveRenderPassFramebufferExtent(false, attachmentExtent, swapchainExtent),
              MobileGL::IntVec2(512, 512));
}

TEST(DirectVulkanSanity, AdvertisesVoxyRequiredRenderingExtensionsWithoutRaisingGLVersion) {
    MobileGL::MG_Backend::DirectVulkan::BackendObject_DirectVulkan backend;
    const auto& rendererInfo = backend.GetRendererInfo().RendererGLInfo;
    const auto& extensions = rendererInfo.Extensions;

    EXPECT_EQ(rendererInfo.TargetGLVersion.Major, 3);
    EXPECT_EQ(rendererInfo.TargetGLVersion.Minor, 3);
    EXPECT_EQ(rendererInfo.TargetGLVersion.Patch, 0);

    EXPECT_NE(std::find(extensions.begin(), extensions.end(), MobileGL::E_GL_ARB_compute_shader),
              extensions.end());
    EXPECT_NE(std::find(extensions.begin(), extensions.end(), MobileGL::E_GL_ARB_shader_storage_buffer_object),
              extensions.end());
    EXPECT_NE(std::find(extensions.begin(), extensions.end(), MobileGL::E_GL_ARB_multi_draw_indirect),
              extensions.end());
    EXPECT_NE(std::find(extensions.begin(), extensions.end(), MobileGL::E_GL_ARB_indirect_parameters),
              extensions.end());
    EXPECT_NE(std::find(extensions.begin(), extensions.end(), MobileGL::E_GL_ARB_shader_draw_parameters),
              extensions.end());
    EXPECT_NE(std::find(extensions.begin(), extensions.end(), MobileGL::E_GL_ARB_gpu_shader_int64),
              extensions.end());
}

TEST(DirectVulkanSanity, ClampsAdvertisedTextureAndDrawBufferLimitsToFrontendState) {
    using namespace MobileGL;

    MG_Backend::DirectVulkan::BackendObject_DirectVulkan backend;

    MG_External::VulkanCapabilities highCaps;
    highCaps.MaxTextureImageUnits = 256;
    highCaps.MaxVertexTextureImageUnits = 256;
    highCaps.MaxComputeTextureImageUnits = 256;
    highCaps.MaxCombinedTextureImageUnits = 256;
    highCaps.MaxDrawBuffers = 128;
    highCaps.MaxColorAttachments = 128;
    backend.ApplyVulkanCapabilitiesForTesting(highCaps);

    const auto& highParams = backend.GetDynamicParameters();
    EXPECT_EQ(highParams.MaxTextureImageUnits, MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS);
    EXPECT_EQ(highParams.MaxVertexTextureImageUnits, MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS);
    EXPECT_EQ(highParams.MaxComputeTextureImageUnits, MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS);
    EXPECT_EQ(highParams.MaxCombinedTextureImageUnits, MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS);
    EXPECT_EQ(highParams.MaxDrawBuffers, MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS);
    EXPECT_EQ(highParams.MaxColorAttachments, MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS);

    MG_External::VulkanCapabilities lowCaps;
    lowCaps.MaxTextureImageUnits = 16;
    lowCaps.MaxVertexTextureImageUnits = 12;
    lowCaps.MaxComputeTextureImageUnits = 10;
    lowCaps.MaxCombinedTextureImageUnits = 20;
    lowCaps.MaxDrawBuffers = 4;
    lowCaps.MaxColorAttachments = 6;
    backend.ApplyVulkanCapabilitiesForTesting(lowCaps);

    const auto& lowParams = backend.GetDynamicParameters();
    EXPECT_EQ(lowParams.MaxTextureImageUnits, 16);
    EXPECT_EQ(lowParams.MaxVertexTextureImageUnits, 12);
    EXPECT_EQ(lowParams.MaxComputeTextureImageUnits, 10);
    EXPECT_EQ(lowParams.MaxCombinedTextureImageUnits, 20);
    EXPECT_EQ(lowParams.MaxDrawBuffers, 4);
    EXPECT_EQ(lowParams.MaxColorAttachments, 6);
}

TEST(DirectVulkanSanity, AdvertisesSubgroupOnlyWhenVulkanReportsUsableSupport) {
    using namespace MobileGL;

    MG_Backend::DirectVulkan::BackendObject_DirectVulkan backend;

    MG_External::VulkanCapabilities unsupportedCaps;
    unsupportedCaps.SupportsShaderSubgroup = false;
    unsupportedCaps.SubgroupSize = 32;
    unsupportedCaps.SubgroupSupportedStages = VK_SHADER_STAGE_COMPUTE_BIT;
    unsupportedCaps.SubgroupSupportedOperations = VK_SUBGROUP_FEATURE_BASIC_BIT;
    backend.ApplyVulkanCapabilitiesForTesting(unsupportedCaps);

    const auto& unsupportedExtensions = backend.GetRendererInfo().RendererGLInfo.Extensions;
    EXPECT_EQ(std::find(unsupportedExtensions.begin(), unsupportedExtensions.end(), E_GL_KHR_shader_subgroup),
              unsupportedExtensions.end());
    EXPECT_EQ(backend.GetDynamicParameters().SubgroupSize, 0u);
    EXPECT_EQ(backend.GetDynamicParameters().SubgroupSupportedStages, 0u);
    EXPECT_EQ(backend.GetDynamicParameters().SubgroupSupportedFeatures, 0u);

    MG_External::VulkanCapabilities supportedCaps;
    supportedCaps.SupportsShaderSubgroup = true;
    supportedCaps.SubgroupSize = 32;
    supportedCaps.SubgroupSupportedStages = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    supportedCaps.SubgroupSupportedOperations =
        VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT | VK_SUBGROUP_FEATURE_QUAD_BIT;
    supportedCaps.SubgroupQuadOperationsInAllStages = true;
    backend.ApplyVulkanCapabilitiesForTesting(supportedCaps);

    const auto& supportedExtensions = backend.GetRendererInfo().RendererGLInfo.Extensions;
    EXPECT_NE(std::find(supportedExtensions.begin(), supportedExtensions.end(), E_GL_KHR_shader_subgroup),
              supportedExtensions.end());
    EXPECT_EQ(backend.GetDynamicParameters().SubgroupSize, 32u);
    EXPECT_EQ(backend.GetDynamicParameters().SubgroupSupportedStages,
              static_cast<Uint32>(GL_FRAGMENT_SHADER_BIT | GL_COMPUTE_SHADER_BIT));
    EXPECT_EQ(backend.GetDynamicParameters().SubgroupSupportedFeatures,
              static_cast<Uint32>(GL_SUBGROUP_FEATURE_BASIC_BIT_KHR |
                                  GL_SUBGROUP_FEATURE_ARITHMETIC_BIT_KHR |
                                  GL_SUBGROUP_FEATURE_QUAD_BIT_KHR));
    EXPECT_TRUE(backend.GetDynamicParameters().SubgroupQuadOperationsInAllStages);
}

TEST(DirectVulkanSanity, KeepsOptionalGpuShaderInt64BranchForVoxyQuadDecode) {
    using namespace MobileGL;

    MG_Backend::pActiveBackendObject = MakeUnique<MG_Backend::DirectVulkan::BackendObject_DirectVulkan>();
    String source = R"(#version 460 core
#extension GL_ARB_gpu_shader_int64 : enable
#ifdef GL_ARB_gpu_shader_int64
uint getLowBits(uint64_t v) {
    return uint(v & uint64_t(0xffu));
}
#else
#error int64 branch should be enabled for DirectVulkan
#endif
void main() {
    gl_Position = vec4(float(getLowBits(uint64_t(0x2au))));
}
)";

    MG_Util::ShaderTranspiler::PreprocessShaderSource(ShaderStage::Vertex, source);
    EXPECT_NE(source.find("#extension GL_ARB_gpu_shader_int64"), String::npos);
    EXPECT_NE(source.find("GL_ARB_gpu_shader_int64"), String::npos);

    auto shaderResult = MG_Util::ShaderTranspiler::ShaderCompiler::CompileShader({
        .shaderType = GL_VERTEX_SHADER,
        .sourceStr = source,
        .flags = MG_Util::ShaderTranspiler::ShaderCompileBits::CompileForOpenGL,
    });
    EXPECT_TRUE(shaderResult) << (shaderResult ? "" : shaderResult.error().log);

    MG_Backend::pActiveBackendObject.reset();
}

TEST(GetterSanity, ReportsKhrSubgroupDynamicParameters) {
    using namespace MobileGL;

    MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();

    MG_Backend::DynamicBackendParameters params;
    params.SubgroupSize = 32;
    params.SubgroupSupportedStages = GL_VERTEX_SHADER_BIT | GL_FRAGMENT_SHADER_BIT | GL_COMPUTE_SHADER_BIT;
    params.SubgroupSupportedFeatures = GL_SUBGROUP_FEATURE_BASIC_BIT_KHR |
                                       GL_SUBGROUP_FEATURE_ARITHMETIC_BIT_KHR |
                                       GL_SUBGROUP_FEATURE_CLUSTERED_BIT_KHR |
                                       GL_SUBGROUP_FEATURE_QUAD_BIT_KHR;
    params.SubgroupQuadOperationsInAllStages = true;
    MG_Backend::pActiveBackendObject = MakeUnique<DynamicParameterBackend>(params);

    GLint intValue = 0;
    MG_Impl::GLImpl::GetIntegerv(GL_SUBGROUP_SIZE_KHR, &intValue);
    EXPECT_EQ(intValue, 32);
    MG_Impl::GLImpl::GetIntegerv(GL_SUBGROUP_SUPPORTED_STAGES_KHR, &intValue);
    EXPECT_EQ(intValue, static_cast<GLint>(params.SubgroupSupportedStages));
    MG_Impl::GLImpl::GetIntegerv(GL_SUBGROUP_SUPPORTED_FEATURES_KHR, &intValue);
    EXPECT_EQ(intValue, static_cast<GLint>(params.SubgroupSupportedFeatures));
    MG_Impl::GLImpl::GetIntegerv(GL_SUBGROUP_QUAD_ALL_STAGES_KHR, &intValue);
    EXPECT_EQ(intValue, GL_TRUE);

    GLint64 int64Value = 0;
    MG_Impl::GLImpl::GetInteger64v(GL_SUBGROUP_SUPPORTED_FEATURES_KHR, &int64Value);
    EXPECT_EQ(int64Value, static_cast<GLint64>(params.SubgroupSupportedFeatures));
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Backend::pActiveBackendObject.reset();
    MG_State::pGLContext.reset();
}

TEST(DirectVulkanSanity, CommandMemoryBarrierMakesIndirectDrawCommandsVisible) {
    using namespace MobileGL;
    using namespace MobileGL::MG_Backend::DirectVulkan;

    const VkMemoryBarrier commandBarrier =
        VulkanRenderer::BuildMemoryBarrierForGlBarriers(GL_COMMAND_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
    EXPECT_NE(commandBarrier.dstAccessMask & VK_ACCESS_INDIRECT_COMMAND_READ_BIT, 0u);

    const VkMemoryBarrier storageOnlyBarrier =
        VulkanRenderer::BuildMemoryBarrierForGlBarriers(GL_SHADER_STORAGE_BARRIER_BIT);
    EXPECT_EQ(storageOnlyBarrier.dstAccessMask & VK_ACCESS_INDIRECT_COMMAND_READ_BIT, 0u);
}

TEST(DirectVulkanSanity, DrawIndexedIndirectCommandMatchesGlAndVulkanLayout) {
    using namespace MobileGL::MG_Backend::DirectVulkan;

    EXPECT_EQ(sizeof(DrawIndexedCmdParam), 20u);
    EXPECT_EQ(offsetof(DrawIndexedCmdParam, indexCount), 0u);
    EXPECT_EQ(offsetof(DrawIndexedCmdParam, instanceCount), 4u);
    EXPECT_EQ(offsetof(DrawIndexedCmdParam, firstIndex), 8u);
    EXPECT_EQ(offsetof(DrawIndexedCmdParam, vertexOffset), 12u);
    EXPECT_EQ(offsetof(DrawIndexedCmdParam, firstInstance), 16u);
}

TEST(DirectVulkanSanity, UndefinedDepthStencilLayoutUsesDontCareForUnclearedAspects) {
    using namespace MobileGL;
    using namespace MobileGL::MG_Backend::DirectVulkan;

    auto noClear = ResolveDepthStencilAttachmentLoadInfo(VK_IMAGE_LAYOUT_UNDEFINED, false, false);
    EXPECT_EQ(noClear.depthLoadOp, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
    EXPECT_EQ(noClear.stencilLoadOp, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
    EXPECT_EQ(noClear.initialLayout, VK_IMAGE_LAYOUT_UNDEFINED);

    auto depthOnlyClear = ResolveDepthStencilAttachmentLoadInfo(VK_IMAGE_LAYOUT_UNDEFINED, true, false);
    EXPECT_EQ(depthOnlyClear.depthLoadOp, VK_ATTACHMENT_LOAD_OP_CLEAR);
    EXPECT_EQ(depthOnlyClear.stencilLoadOp, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
    EXPECT_EQ(depthOnlyClear.initialLayout, VK_IMAGE_LAYOUT_UNDEFINED);

    auto knownLayout = ResolveDepthStencilAttachmentLoadInfo(
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, false, false);
    EXPECT_EQ(knownLayout.depthLoadOp, VK_ATTACHMENT_LOAD_OP_LOAD);
    EXPECT_EQ(knownLayout.stencilLoadOp, VK_ATTACHMENT_LOAD_OP_LOAD);
    EXPECT_EQ(knownLayout.initialLayout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
}

TEST(DirectVulkanSanity, SampledDepthStencilViewUsesSingleDepthAspect) {
    using namespace MobileGL::MG_Backend::DirectVulkan;

    EXPECT_EQ(VkTextureManager::ResolveSampledImageViewAspectMask(VK_IMAGE_ASPECT_COLOR_BIT),
              VK_IMAGE_ASPECT_COLOR_BIT);
    EXPECT_EQ(VkTextureManager::ResolveSampledImageViewAspectMask(VK_IMAGE_ASPECT_DEPTH_BIT),
              VK_IMAGE_ASPECT_DEPTH_BIT);
    EXPECT_EQ(VkTextureManager::ResolveSampledImageViewAspectMask(VK_IMAGE_ASPECT_STENCIL_BIT),
              VK_IMAGE_ASPECT_STENCIL_BIT);
    EXPECT_EQ(VkTextureManager::ResolveSampledImageViewAspectMask(
                  VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT),
              VK_IMAGE_ASPECT_DEPTH_BIT);
}

TEST(RenderStateSanity, ProvokingVertexUpdatesStateAndValidatesEnum) {
    using namespace MobileGL;

    MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();
    MG_Backend::pActiveBackendObject = MakeUnique<MG_Backend::DirectGLES::BackendObject_DirectGLES>();

    GLint mode = 0;
    MG_Impl::GLImpl::GetIntegerv(GL_PROVOKING_VERTEX, &mode);
    EXPECT_EQ(mode, GL_LAST_VERTEX_CONVENTION);

    const Uint initialVersion = MG_State::pGLContext->GetRenderStateParametersVersion();
    MG_Impl::GLImpl::ProvokingVertex(GL_FIRST_VERTEX_CONVENTION);
    MG_Impl::GLImpl::GetIntegerv(GL_PROVOKING_VERTEX, &mode);
    EXPECT_EQ(mode, GL_FIRST_VERTEX_CONVENTION);
    EXPECT_GT(MG_State::pGLContext->GetRenderStateParametersVersion(), initialVersion);

    const Uint updatedVersion = MG_State::pGLContext->GetRenderStateParametersVersion();
    MG_Impl::GLImpl::ProvokingVertex(GL_FIRST_VERTEX_CONVENTION);
    EXPECT_EQ(MG_State::pGLContext->GetRenderStateParametersVersion(), updatedVersion);

    MG_Impl::GLImpl::ProvokingVertex(GL_TRIANGLES);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_INVALID_ENUM);
    MG_Impl::GLImpl::GetIntegerv(GL_PROVOKING_VERTEX, &mode);
    EXPECT_EQ(mode, GL_FIRST_VERTEX_CONVENTION);

    MG_Backend::pActiveBackendObject.reset();
    MG_State::pGLContext.reset();
}

TEST(LogSanity, UsesEnvOverrideForFilePath) {
    namespace fs = std::filesystem;

    MobileGL::MG_Util::Debug::Close();

    const fs::path logPath = fs::temp_directory_path() / "mobilegl-log-env-override-test.log";
    const std::string message = "mobilegl-log-env-override-regression";
    fs::remove(logPath);

    SetEnvVar("MOBILEGL_LOG_FILE_PATH", logPath.string().c_str());
    MobileGL::MG_Util::Debug::Log("INFO", ANDROID_LOG_INFO, "%s", message.c_str());
    MobileGL::MG_Util::Debug::Close();
    UnsetEnvVar("MOBILEGL_LOG_FILE_PATH");

    {
        std::ifstream logFile(logPath);
        ASSERT_TRUE(logFile.good());

        const std::string contents((std::istreambuf_iterator<char>(logFile)), std::istreambuf_iterator<char>());
        EXPECT_NE(contents.find(message), std::string::npos);
    }

    fs::remove(logPath);
}
