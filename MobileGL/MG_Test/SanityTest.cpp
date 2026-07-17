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
#include <MG_Impl/GLImpl/Texture/GL_Texture.h>
#include <MG_Impl/GLImpl/VertexArray/Validators.h>
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

    MobileGL::SizeT CountOccurrences(const MobileGL::String& haystack, const MobileGL::String& needle) {
        MobileGL::SizeT count = 0;
        for (MobileGL::SizeT pos = haystack.find(needle); pos != MobileGL::String::npos;
             pos = haystack.find(needle, pos + needle.size())) {
            ++count;
        }
        return count;
    }

    // Snapshots the DirectGLES capability globals on construction and restores them on
    // destruction, so tests that mutate g_GLESCapabilities cannot leak state into later
    // tests even if an assertion or exception unwinds the test body early.
    struct ScopedGLESCapabilitiesOverride {
        ScopedGLESCapabilitiesOverride():
            m_snapshot(MobileGL::MG_Backend::DirectGLES::g_GLESCapabilities) {}
        ~ScopedGLESCapabilitiesOverride() {
            MobileGL::MG_Backend::DirectGLES::g_GLESCapabilities = m_snapshot;
        }
        ScopedGLESCapabilitiesOverride(const ScopedGLESCapabilitiesOverride&) = delete;
        ScopedGLESCapabilitiesOverride& operator=(const ScopedGLESCapabilitiesOverride&) = delete;

    private:
        MobileGL::MG_External::GLESCapabilities m_snapshot;
    };

    struct TextureBindCall {
        GLenum target;
        GLuint texture;
    };

    MobileGL::Vector<TextureBindCall>* g_textureBindCalls = nullptr;
    GLuint g_nextBackendTextureId = 73;

    void RecordTextureBind(GLenum target, GLuint texture) {
        if (g_textureBindCalls) {
            g_textureBindCalls->push_back({target, texture});
        }
    }

    void GenerateBackendTextures(GLsizei count, GLuint* textures) {
        for (GLsizei i = 0; i < count; ++i) {
            textures[i] = g_nextBackendTextureId++;
        }
    }

    void DeleteBackendTextures(GLsizei, const GLuint*) {}

    GLenum NoBackendError() {
        return GL_NO_ERROR;
    }

    // Isolates the DirectGLES globals touched by the binding-cache regression test. The test
    // installs only the native ES entry points needed to construct/bind a backend texture and
    // restores the process-wide state even when a gtest assertion unwinds the test body.
    struct ScopedDirectGLESTextureBindings {
        ScopedDirectGLESTextureBindings():
            previousContext(MobileGL::Move(MobileGL::MG_State::pGLContext)),
            previousFunctions(MobileGL::MG_Backend::DirectGLES::g_GLESFuncs),
            previousActiveUnit(MobileGL::MG_Backend::DirectGLES::TextureImpl::g_activeTextureUnit),
            previousCache(MobileGL::MG_Backend::DirectGLES::TextureImpl::g_boundTexturesCache),
            previousRegistry(MobileGL::MG_Backend::DirectGLES::TextureImpl::g_backendTextureObjects) {
            MobileGL::MG_State::pGLContext = MobileGL::MakeUnique<MobileGL::MG_State::GLState::GLContext>();
            MobileGL::MG_Backend::DirectGLES::TextureImpl::g_activeTextureUnit = 0;
            MobileGL::MG_Backend::DirectGLES::TextureImpl::g_boundTexturesCache = {};
            MobileGL::MG_Backend::DirectGLES::TextureImpl::g_backendTextureObjects = {};

            MobileGL::MG_External::GLESFunctionsTable functions{};
            functions.glBindTexture = RecordTextureBind;
            functions.glDeleteTextures = DeleteBackendTextures;
            functions.glGenTextures = GenerateBackendTextures;
            functions.glGetError = NoBackendError;
            MobileGL::MG_Backend::DirectGLES::SetGLESFuncsTable(functions);
            g_textureBindCalls = &bindCalls;
        }

        ~ScopedDirectGLESTextureBindings() {
            g_textureBindCalls = nullptr;
            MobileGL::MG_Backend::DirectGLES::TextureImpl::g_boundTexturesCache = {};
            MobileGL::MG_Backend::DirectGLES::TextureImpl::g_backendTextureObjects = previousRegistry;
            MobileGL::MG_Backend::DirectGLES::SetGLESFuncsTable(previousFunctions);
            MobileGL::MG_Backend::DirectGLES::TextureImpl::g_activeTextureUnit = previousActiveUnit;
            MobileGL::MG_Backend::DirectGLES::TextureImpl::g_boundTexturesCache = previousCache;
            MobileGL::MG_State::pGLContext = MobileGL::Move(previousContext);
        }

        ScopedDirectGLESTextureBindings(const ScopedDirectGLESTextureBindings&) = delete;
        ScopedDirectGLESTextureBindings& operator=(const ScopedDirectGLESTextureBindings&) = delete;

        MobileGL::Vector<TextureBindCall> bindCalls;

    private:
        MobileGL::UniquePtr<MobileGL::MG_State::GLState::GLContext> previousContext;
        MobileGL::MG_External::GLESFunctionsTable previousFunctions;
        MobileGL::Uint previousActiveUnit;
        decltype(MobileGL::MG_Backend::DirectGLES::TextureImpl::g_boundTexturesCache) previousCache;
        decltype(MobileGL::MG_Backend::DirectGLES::TextureImpl::g_backendTextureObjects) previousRegistry;
    };
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

TEST(DirectGLESSanity, BindingZeroClearsPreviousNativeTextureBinding) {
    using namespace MobileGL;
    namespace DirectGLES = MG_Backend::DirectGLES;

    ScopedDirectGLESTextureBindings state;

    GLuint frontendTexture = 0;
    MG_Impl::GLImpl::GenTextures(1, &frontendTexture);
    ASSERT_NE(frontendTexture, 0u);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, frontendTexture);
    const auto& frontendTextureObject = MG_State::pGLContext->GetTextureUnitObject(0)
                                            .GetBindingSlot(TextureTarget::Texture2D)
                                            .GetBoundObject();
    ASSERT_NE(frontendTextureObject, nullptr);
    ASSERT_EQ(frontendTextureObject->GetExternalIndex(), frontendTexture);

    auto& backendTexture = DirectGLES::TextureImpl::g_backendTextureObjects.GetOrCreate(frontendTextureObject);
    backendTexture = MakeShared<DirectGLES::TextureImpl::BackendTextureObject>();
    const GLuint backendTextureId = backendTexture->GetBackendTextureId();

    DirectGLES::BindCurrentTextures();
    ASSERT_EQ(state.bindCalls.size(), 1u);
    EXPECT_EQ(state.bindCalls[0].target, GL_TEXTURE_2D);
    EXPECT_EQ(state.bindCalls[0].texture, backendTextureId);

    // The default 1D slot maps to the same native ES target as 2D. It must not clear and force a
    // redundant rebind while the real 2D frontend object remains current.
    DirectGLES::BindCurrentTextures();
    EXPECT_EQ(state.bindCalls.size(), 1u);

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, 0);
    ASSERT_TRUE(MG_State::GLState::IsUndefinedDefaultTexture(
        MG_State::pGLContext->GetTextureUnitObject(0)
            .GetBindingSlot(TextureTarget::Texture2D)
            .GetBoundObject()
            .get()));

    DirectGLES::BindCurrentTextures();

    ASSERT_EQ(state.bindCalls.size(), 2u);
    EXPECT_EQ(state.bindCalls[1].target, GL_TEXTURE_2D);
    EXPECT_EQ(state.bindCalls[1].texture, 0u);
    EXPECT_EQ(DirectGLES::TextureImpl::g_boundTexturesCache[0][static_cast<SizeT>(TextureTarget::Texture2D)],
              nullptr);
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

TEST(DirectGLESSanity, RebasesInstanceIdWhenIndirectDrawsLeakBaseInstance) {
    const ScopedGLESCapabilitiesOverride capsGuard;
    auto& caps = MobileGL::MG_Backend::DirectGLES::g_GLESCapabilities;
    caps.IndirectDrawInstanceIdIncludesBaseInstance = true;
    // Deliberately not the GLESCapabilities default (8): the injected block must land at
    // MaxShaderStorageBufferBindings - 1 = 12, so a regression that stops reading the
    // probed cap and falls back to the struct default would surface as "binding = 7".
    caps.MaxShaderStorageBufferBindings = 13;

    const MobileGL::String source = R"(#version 310 es
highp int mg_BaseInstanceLowered;
void main() {
    int instance = gl_InstanceID + mg_BaseInstanceLowered;
    gl_Position = vec4(float(instance));
}
)";

    const auto rewritten = MobileGL::MG_Backend::DirectGLES::PromoteDrawParameterGlobalsToUniforms(
        source, GL_VERTEX_SHADER);

    EXPECT_NE(rewritten.find("int instance = mg_ZeroBasedInstanceID + mg_BaseInstanceLowered;"),
              MobileGL::String::npos);
    EXPECT_NE(rewritten.find("#define mg_ZeroBasedInstanceID (gl_InstanceID - ((mg_BaseInstanceWordIndex >= 0) ? "
                             "int(mg_indirectWords[uint(mg_BaseInstanceWordIndex)]) : 0))"),
              MobileGL::String::npos);
    EXPECT_NE(rewritten.find(
                  "layout(std430, binding = 12) readonly buffer mg_IndirectParams { highp uint mg_indirectWords[]; };"),
              MobileGL::String::npos);
    // The one inside the #define machinery must be the only surviving gl_InstanceID.
    EXPECT_EQ(CountOccurrences(rewritten, "gl_InstanceID"), 1u);
}

TEST(DirectGLESSanity, KeepsInstanceIdWhenIndirectDrawsAreConforming) {
    const ScopedGLESCapabilitiesOverride capsGuard;
    auto& caps = MobileGL::MG_Backend::DirectGLES::g_GLESCapabilities;
    caps.IndirectDrawInstanceIdIncludesBaseInstance = false;
    caps.MaxShaderStorageBufferBindings = 13;

    const MobileGL::String source = R"(#version 310 es
highp int mg_BaseInstanceLowered;
void main() {
    int instance = gl_InstanceID + mg_BaseInstanceLowered;
    gl_Position = vec4(float(instance));
}
)";

    const auto rewritten = MobileGL::MG_Backend::DirectGLES::PromoteDrawParameterGlobalsToUniforms(
        source, GL_VERTEX_SHADER);

    EXPECT_EQ(rewritten.find("mg_ZeroBasedInstanceID"), MobileGL::String::npos);
    EXPECT_NE(rewritten.find("int instance = gl_InstanceID + mg_BaseInstanceLowered;"), MobileGL::String::npos);
}

TEST(DirectGLESSanity, LeavesDrawParameterGlobalsAloneOutsideVertexShaders) {
    const ScopedGLESCapabilitiesOverride capsGuard;
    auto& caps = MobileGL::MG_Backend::DirectGLES::g_GLESCapabilities;
    caps.IndirectDrawInstanceIdIncludesBaseInstance = true;
    caps.MaxShaderStorageBufferBindings = 13;

    const MobileGL::String source =
        "#version 310 es\nhighp int mg_BaseInstanceLowered;\nint value = gl_InstanceID + mg_BaseInstanceLowered;\n";

    const auto rewritten = MobileGL::MG_Backend::DirectGLES::PromoteDrawParameterGlobalsToUniforms(
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
    // Per-stage sampler limits clamp to the desktop-conventional per-stage cap (kept well under
    // Blaze3D's 128-entry TEXTURES[] so Iris' unbind loop cannot index out of bounds); the combined
    // limit clamps to the full texture-unit array capacity.
    EXPECT_EQ(highParams.MaxTextureImageUnits, MG_State::GLState::TextureState::MAX_PER_STAGE_TEXTURE_IMAGE_UNITS);
    EXPECT_EQ(highParams.MaxVertexTextureImageUnits, MG_State::GLState::TextureState::MAX_PER_STAGE_TEXTURE_IMAGE_UNITS);
    EXPECT_EQ(highParams.MaxComputeTextureImageUnits, MG_State::GLState::TextureState::MAX_PER_STAGE_TEXTURE_IMAGE_UNITS);
    EXPECT_LE(highParams.MaxTextureImageUnits, 128);
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

// GL_MAX_VERTEX_ATTRIBS must follow the backend but never exceed the state layer's current-value
// storage: the DirectVulkan draw path indexes that array by shader input location, so advertising more
// than it can hold is an out-of-bounds read waiting to happen.
TEST(GetterSanity, ClampsMaxVertexAttribsToCurrentValueStorageCapacity) {
    using namespace MobileGL;
    constexpr GLint capacity = MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS;

    MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();

    // A driver reporting more attributes than MobileGL can store gets clamped.
    {
        MG_Backend::DynamicBackendParameters params;
        params.MaxVertexAttribs = capacity * 2;
        MG_Backend::pActiveBackendObject = MakeUnique<DynamicParameterBackend>(params);

        EXPECT_EQ(MG_Impl::GLImpl::VertexArrayImpl::GetMaxVertexAttribs(), static_cast<Uint>(capacity));
        GLint reported = 0;
        MG_Impl::GLImpl::GetIntegerv(GL_MAX_VERTEX_ATTRIBS, &reported);
        EXPECT_EQ(reported, capacity);
        MG_Backend::pActiveBackendObject.reset();
    }

    // A driver below the capacity is followed exactly, and validation enforces that same bound.
    {
        MG_Backend::DynamicBackendParameters params;
        params.MaxVertexAttribs = 16;
        MG_Backend::pActiveBackendObject = MakeUnique<DynamicParameterBackend>(params);

        EXPECT_EQ(MG_Impl::GLImpl::VertexArrayImpl::GetMaxVertexAttribs(), 16u);
        GLint reported = 0;
        MG_Impl::GLImpl::GetIntegerv(GL_MAX_VERTEX_ATTRIBS, &reported);
        EXPECT_EQ(reported, 16);

        MG_State::pGLContext->ClearErrors();
        EXPECT_FALSE(MG_Impl::GLImpl::VertexArrayImpl::ValidateVertexAttributeIndex(16));
        EXPECT_TRUE(MG_State::pGLContext->HasGLError());
        MG_State::pGLContext->ClearErrors();
        EXPECT_TRUE(MG_Impl::GLImpl::VertexArrayImpl::ValidateVertexAttributeIndex(15));
        EXPECT_FALSE(MG_State::pGLContext->HasGLError());

        MG_Backend::pActiveBackendObject.reset();
    }

    // With no active backend the storage capacity is the bound, and nothing dereferences a null backend.
    EXPECT_EQ(MG_Impl::GLImpl::VertexArrayImpl::GetMaxVertexAttribs(), static_cast<Uint>(capacity));

    MG_State::pGLContext.reset();
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

// ---- Pure-state entry points: glHint / glPointParameter* / glPixelStoref / glGetDoublev -----------

TEST(RenderStateSanity, HintStoresAndReadsBack) {
    using namespace MobileGL;
    using namespace MobileGL::MG_Impl::GLImpl;
    MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();

    // Default is GL_DONT_CARE.
    GLint value = -1;
    GetIntegerv(GL_LINE_SMOOTH_HINT, &value);
    EXPECT_EQ(value, GL_DONT_CARE);

    // Each of the 4 core targets round-trips.
    Hint(GL_LINE_SMOOTH_HINT, GL_NICEST);
    Hint(GL_POLYGON_SMOOTH_HINT, GL_FASTEST);
    Hint(GL_TEXTURE_COMPRESSION_HINT, GL_NICEST);
    Hint(GL_FRAGMENT_SHADER_DERIVATIVE_HINT, GL_FASTEST);
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    GetIntegerv(GL_LINE_SMOOTH_HINT, &value);
    EXPECT_EQ(value, GL_NICEST);
    GetIntegerv(GL_POLYGON_SMOOTH_HINT, &value);
    EXPECT_EQ(value, GL_FASTEST);
    GetIntegerv(GL_TEXTURE_COMPRESSION_HINT, &value);
    EXPECT_EQ(value, GL_NICEST);
    GetIntegerv(GL_FRAGMENT_SHADER_DERIVATIVE_HINT, &value);
    EXPECT_EQ(value, GL_FASTEST);

    // glGetBooleanv on a hint is always GL_TRUE (all hint enums are non-zero).
    GLboolean b = GL_FALSE;
    GetBooleanv(GL_LINE_SMOOTH_HINT, &b);
    EXPECT_EQ(b, GL_TRUE);

    // A compatibility-only target and a bad mode both raise GL_INVALID_ENUM and change nothing.
    Hint(GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);
    EXPECT_EQ(GetError(), GL_INVALID_ENUM);
    Hint(GL_LINE_SMOOTH_HINT, GL_LINEAR);
    EXPECT_EQ(GetError(), GL_INVALID_ENUM);
    GetIntegerv(GL_LINE_SMOOTH_HINT, &value);
    EXPECT_EQ(value, GL_NICEST); // unchanged by the failed calls

    MG_State::pGLContext.reset();
}

TEST(RenderStateSanity, PointParameterStoresAndReadsBack) {
    using namespace MobileGL;
    using namespace MobileGL::MG_Impl::GLImpl;
    MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();

    // Defaults: fade threshold 1.0, coord origin GL_UPPER_LEFT.
    GLfloat f = -1.0f;
    GetFloatv(GL_POINT_FADE_THRESHOLD_SIZE, &f);
    EXPECT_FLOAT_EQ(f, 1.0f);
    GLint origin = -1;
    GetIntegerv(GL_POINT_SPRITE_COORD_ORIGIN, &origin);
    EXPECT_EQ(origin, GL_UPPER_LEFT);

    // Scalar float sets the fade threshold; GetFloatv keeps the fractional part, GetIntegerv rounds.
    PointParameterf(GL_POINT_FADE_THRESHOLD_SIZE, 2.5f);
    GetFloatv(GL_POINT_FADE_THRESHOLD_SIZE, &f);
    EXPECT_FLOAT_EQ(f, 2.5f);
    GLint fi = 0;
    GetIntegerv(GL_POINT_FADE_THRESHOLD_SIZE, &fi);
    EXPECT_EQ(fi, 3); // 2.5 rounds to nearest (to even or up; lround gives 3)

    // The v-form reads params[0]; the integer form sets the coord-origin enum.
    const GLint lowerLeft = GL_LOWER_LEFT;
    PointParameteriv(GL_POINT_SPRITE_COORD_ORIGIN, &lowerLeft);
    GetIntegerv(GL_POINT_SPRITE_COORD_ORIGIN, &origin);
    EXPECT_EQ(origin, GL_LOWER_LEFT);
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    // Errors: negative fade -> GL_INVALID_VALUE; bad coord-origin -> GL_INVALID_ENUM; compat pname ->
    // GL_INVALID_ENUM. None change state.
    PointParameterf(GL_POINT_FADE_THRESHOLD_SIZE, -1.0f);
    EXPECT_EQ(GetError(), GL_INVALID_VALUE);
    PointParameteri(GL_POINT_SPRITE_COORD_ORIGIN, GL_FASTEST);
    EXPECT_EQ(GetError(), GL_INVALID_ENUM);
    PointParameterf(GL_POINT_SIZE_MIN, 0.0f);
    EXPECT_EQ(GetError(), GL_INVALID_ENUM);

    GetFloatv(GL_POINT_FADE_THRESHOLD_SIZE, &f);
    EXPECT_FLOAT_EQ(f, 2.5f); // unchanged
    GetIntegerv(GL_POINT_SPRITE_COORD_ORIGIN, &origin);
    EXPECT_EQ(origin, GL_LOWER_LEFT); // unchanged

    MG_State::pGLContext.reset();
}

TEST(RenderStateSanity, PixelStorefRoundsAndZeroTestsBooleans) {
    using namespace MobileGL;
    using namespace MobileGL::MG_Impl::GLImpl;
    MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();

    // Integer pname: round to nearest.
    PixelStoref(GL_UNPACK_ROW_LENGTH, 7.4f);
    GLint iv = 0;
    GetIntegerv(GL_UNPACK_ROW_LENGTH, &iv);
    EXPECT_EQ(iv, 7);
    PixelStoref(GL_UNPACK_ROW_LENGTH, 7.6f);
    GetIntegerv(GL_UNPACK_ROW_LENGTH, &iv);
    EXPECT_EQ(iv, 8);

    // Boolean pname: a fractional value must map to TRUE via a zero-test, NOT round-to-zero.
    PixelStoref(GL_PACK_SWAP_BYTES, 0.4f);
    GLboolean bv = GL_FALSE;
    GetBooleanv(GL_PACK_SWAP_BYTES, &bv);
    EXPECT_EQ(bv, GL_TRUE);
    PixelStoref(GL_PACK_SWAP_BYTES, 0.0f);
    GetBooleanv(GL_PACK_SWAP_BYTES, &bv);
    EXPECT_EQ(bv, GL_FALSE);

    // glPixelStoref matches glPixelStorei for an integer pname.
    PixelStorei(GL_PACK_ALIGNMENT, 8);
    GLint viaI = 0;
    GetIntegerv(GL_PACK_ALIGNMENT, &viaI);
    PixelStoref(GL_PACK_ALIGNMENT, 8.0f);
    GLint viaF = 0;
    GetIntegerv(GL_PACK_ALIGNMENT, &viaF);
    EXPECT_EQ(viaI, viaF);
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    MG_State::pGLContext.reset();
}

TEST(RenderStateSanity, GetDoublevMatchesGetFloatvWidened) {
    using namespace MobileGL;
    using namespace MobileGL::MG_Impl::GLImpl;
    MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();

    // Single-component pname.
    PointParameterf(GL_POINT_FADE_THRESHOLD_SIZE, 2.5f);
    GLdouble d1[4] = {-1, -1, -1, -1};
    GetDoublev(GL_POINT_FADE_THRESHOLD_SIZE, d1);
    EXPECT_DOUBLE_EQ(d1[0], 2.5);
    EXPECT_DOUBLE_EQ(d1[1], -1.0); // second component untouched (1-component pname)

    // 2-component pname: GL_DEPTH_RANGE (default 0..1).
    GLdouble d2[2] = {-1, -1};
    GetDoublev(GL_DEPTH_RANGE, d2);
    EXPECT_DOUBLE_EQ(d2[0], 0.0);
    EXPECT_DOUBLE_EQ(d2[1], 1.0);

    // 4-component pname: GL_COLOR_CLEAR_VALUE (default 0,0,0,1) matches GetFloatv widened.
    GLfloat cf[4] = {};
    GetFloatv(GL_COLOR_CLEAR_VALUE, cf);
    GLdouble cd[4] = {};
    GetDoublev(GL_COLOR_CLEAR_VALUE, cd);
    for (int i = 0; i < 4; ++i) EXPECT_DOUBLE_EQ(cd[i], static_cast<GLdouble>(cf[i]));

    // Null params -> GL_INVALID_VALUE.
    GetDoublev(GL_DEPTH_RANGE, nullptr);
    EXPECT_EQ(GetError(), GL_INVALID_VALUE);

    MG_State::pGLContext.reset();
}

TEST(RenderStateSanity, ClampColorStoresAndReadsBack) {
    using namespace MobileGL;
    using namespace MobileGL::MG_Impl::GLImpl;
    MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();

    // Default GL_CLAMP_READ_COLOR is GL_FIXED_ONLY (NOT GL_TRUE/GL_FALSE). glGetIntegerv is the only
    // getter that faithfully round-trips the tri-state.
    GLint value = -1;
    GetIntegerv(GL_CLAMP_READ_COLOR, &value);
    EXPECT_EQ(value, GL_FIXED_ONLY);

    // All three legal clamp values round-trip.
    ClampColor(GL_CLAMP_READ_COLOR, GL_TRUE);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
    GetIntegerv(GL_CLAMP_READ_COLOR, &value);
    EXPECT_EQ(value, GL_TRUE);

    ClampColor(GL_CLAMP_READ_COLOR, GL_FALSE);
    GetIntegerv(GL_CLAMP_READ_COLOR, &value);
    EXPECT_EQ(value, GL_FALSE);

    // GL_FIXED_ONLY MUST be accepted: the Khronos man page's Errors section wrongly omits it, but the
    // spec lists it as legal and it is the default. A man-page-faithful implementation would reject
    // this call -- this assertion is the guard against that regression.
    ClampColor(GL_CLAMP_READ_COLOR, GL_FIXED_ONLY);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
    GetIntegerv(GL_CLAMP_READ_COLOR, &value);
    EXPECT_EQ(value, GL_FIXED_ONLY);

    // glGetBooleanv converts nonzero to GL_TRUE, so GL_FIXED_ONLY reads back as GL_TRUE (and cannot be
    // distinguished from GL_TRUE); only GL_FALSE reads GL_FALSE.
    GLboolean b = GL_FALSE;
    GetBooleanv(GL_CLAMP_READ_COLOR, &b);
    EXPECT_EQ(b, GL_TRUE);
    ClampColor(GL_CLAMP_READ_COLOR, GL_FALSE);
    GetBooleanv(GL_CLAMP_READ_COLOR, &b);
    EXPECT_EQ(b, GL_FALSE);

    // Errors leave state unchanged. A non-GL_CLAMP_READ_COLOR target (here GL_FRONT, standing in for
    // any illegal/compat target) and a bad clamp value both raise GL_INVALID_ENUM.
    ClampColor(GL_FRONT, GL_TRUE);
    EXPECT_EQ(GetError(), GL_INVALID_ENUM);
    ClampColor(GL_CLAMP_READ_COLOR, GL_NICEST);
    EXPECT_EQ(GetError(), GL_INVALID_ENUM);
    GetIntegerv(GL_CLAMP_READ_COLOR, &value);
    EXPECT_EQ(value, GL_FALSE); // unchanged by the failed calls

    MG_State::pGLContext.reset();
}

TEST(RenderStateSanity, PolygonModeStoresAndReadsBack) {
    using namespace MobileGL;
    using namespace MobileGL::MG_Impl::GLImpl;
    MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();

    // GL_POLYGON_MODE reports TWO values (front, back); default GL_FILL for both.
    GLint mode[2] = {-1, -1};
    GetIntegerv(GL_POLYGON_MODE, mode);
    EXPECT_EQ(mode[0], GL_FILL);
    EXPECT_EQ(mode[1], GL_FILL);

    // Core sets both faces together; each legal mode round-trips into both slots.
    PolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
    GetIntegerv(GL_POLYGON_MODE, mode);
    EXPECT_EQ(mode[0], GL_LINE);
    EXPECT_EQ(mode[1], GL_LINE);

    PolygonMode(GL_FRONT_AND_BACK, GL_POINT);
    GetIntegerv(GL_POLYGON_MODE, mode);
    EXPECT_EQ(mode[0], GL_POINT);
    EXPECT_EQ(mode[1], GL_POINT);

    // Core rejects separate faces: GL_FRONT/GL_BACK were removed in 3.1 core -> GL_INVALID_ENUM, no
    // state change. (Some desktop drivers leniently accept them; this guards against copying that.)
    PolygonMode(GL_FRONT, GL_FILL);
    EXPECT_EQ(GetError(), GL_INVALID_ENUM);
    PolygonMode(GL_BACK, GL_FILL);
    EXPECT_EQ(GetError(), GL_INVALID_ENUM);
    // A bad mode also raises GL_INVALID_ENUM.
    PolygonMode(GL_FRONT_AND_BACK, GL_LINEAR);
    EXPECT_EQ(GetError(), GL_INVALID_ENUM);

    GetIntegerv(GL_POLYGON_MODE, mode);
    EXPECT_EQ(mode[0], GL_POINT); // unchanged by the failed calls
    EXPECT_EQ(mode[1], GL_POINT);

    MG_State::pGLContext.reset();
}

TEST(RenderStateSanity, ColorMaskIndexedStoresAndReadsBack) {
    using namespace MobileGL;
    using namespace MobileGL::MG_Impl::GLImpl;
    constexpr GLuint kMaxDrawBuffers = MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS;
    MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();

    // Default: every draw buffer's writemask is all-true.
    GLboolean b0[4] = {GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE};
    GetBooleanv(GL_COLOR_WRITEMASK, b0);
    EXPECT_EQ(b0[0], GL_TRUE); EXPECT_EQ(b0[1], GL_TRUE);
    EXPECT_EQ(b0[2], GL_TRUE); EXPECT_EQ(b0[3], GL_TRUE);
    GLboolean bi[4] = {GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE};
    GetBooleani_v(GL_COLOR_WRITEMASK, 3, bi);
    EXPECT_EQ(bi[0], GL_TRUE); EXPECT_EQ(bi[3], GL_TRUE);
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    // glColorMaski sets ONLY the addressed draw buffer; buffer 0 stays untouched, and the non-indexed
    // glGetBooleanv still reports buffer 0.
    ColorMaski(2, GL_FALSE, GL_TRUE, GL_FALSE, GL_TRUE);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
    GetBooleani_v(GL_COLOR_WRITEMASK, 2, bi);
    EXPECT_EQ(bi[0], GL_FALSE); EXPECT_EQ(bi[1], GL_TRUE);
    EXPECT_EQ(bi[2], GL_FALSE); EXPECT_EQ(bi[3], GL_TRUE);
    GetBooleanv(GL_COLOR_WRITEMASK, b0);
    EXPECT_EQ(b0[0], GL_TRUE); EXPECT_EQ(b0[1], GL_TRUE); // buffer 0 unchanged by ColorMaski(2, ...)

    // GLboolean coercion: any nonzero byte enables the component (NOT == GL_TRUE).
    ColorMaski(1, static_cast<GLboolean>(2), static_cast<GLboolean>(0),
               static_cast<GLboolean>(2), static_cast<GLboolean>(0));
    GetBooleani_v(GL_COLOR_WRITEMASK, 1, bi);
    EXPECT_EQ(bi[0], GL_TRUE);  // 2 -> TRUE
    EXPECT_EQ(bi[1], GL_FALSE); // 0 -> FALSE
    EXPECT_EQ(bi[2], GL_TRUE);
    EXPECT_EQ(bi[3], GL_FALSE);

    // glColorMask (non-indexed) broadcasts to EVERY draw buffer, overwriting the per-buffer masks.
    ColorMask(GL_FALSE, GL_FALSE, GL_TRUE, GL_TRUE);
    GetBooleani_v(GL_COLOR_WRITEMASK, 2, bi);
    EXPECT_EQ(bi[0], GL_FALSE); EXPECT_EQ(bi[2], GL_TRUE); // buffer 2 was overwritten by the broadcast
    GetBooleani_v(GL_COLOR_WRITEMASK, 1, bi);
    EXPECT_EQ(bi[0], GL_FALSE); EXPECT_EQ(bi[3], GL_TRUE);

    // Out-of-range index -> GL_INVALID_VALUE, no state change.
    ColorMaski(kMaxDrawBuffers, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    EXPECT_EQ(GetError(), GL_INVALID_VALUE);
    GetBooleani_v(GL_COLOR_WRITEMASK, 2, bi);
    EXPECT_EQ(bi[0], GL_FALSE); // unchanged (still the broadcast value)
    EXPECT_EQ(bi[2], GL_TRUE);

    MG_State::pGLContext.reset();
}

TEST(RenderStateSanity, PrimitiveRestartIndexStoresAndReadsBack) {
    using namespace MobileGL;
    using namespace MobileGL::MG_Impl::GLImpl;
    MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();

    // Default is 0.
    GLint value = -1;
    GetIntegerv(GL_PRIMITIVE_RESTART_INDEX, &value);
    EXPECT_EQ(value, 0);

    // Any GLuint round-trips and generates no error.
    PrimitiveRestartIndex(0xFFFFu);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
    GetIntegerv(GL_PRIMITIVE_RESTART_INDEX, &value);
    EXPECT_EQ(value, 0xFFFF);

    // The full 32-bit range round-trips (read back as the same bit pattern).
    PrimitiveRestartIndex(0xFFFFFFFFu);
    GetIntegerv(GL_PRIMITIVE_RESTART_INDEX, &value);
    EXPECT_EQ(static_cast<GLuint>(value), 0xFFFFFFFFu);
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    MG_State::pGLContext.reset();
}
