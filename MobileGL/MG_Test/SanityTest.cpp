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
#include <MG_Impl/GLImpl/Buffer/GL_Buffer.h>
#include <MG_Impl/GLImpl/Getter/GL_Getter.h>
#include <MG_Impl/GLImpl/RenderState/GL_RenderState.h>
#include <MG_Impl/GLImpl/Texture/GL_Texture.h>
#include <MG_Impl/GLImpl/VertexArray/Validators.h>
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/FramebufferState/FramebufferObject.h>
#include <MG_State/GLState/TextureState/TextureState.h>
#include <MG_Test/ScopedPipeVerb.h>
#include <MG_Backend/DirectVulkan/Renderer/ProgramFactory.h>
#include <MG_Backend/DirectVulkan/Renderer/UniformManager.h>
#include <MG_Backend/DirectVulkan/Renderer/VkRenderPassManager.h>
#include <MG_Backend/DirectVulkan/Renderer/VkTextureManager.h>
#include <MG_Backend/DirectVulkan/Renderer/VulkanRenderer.h>
#include <MG_Util/Math/HalfFloat.h>
#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>
#include <MG_Util/ShaderTranspiler/CompileEnv.h>
#include <MG_Util/ShaderTranspiler/ShaderSourceProcessor.h>
#include <MG_Util/Debug/Log.h>
#include <MG_Util/BackendLoaders/OpenGL/Loader.h>
#include <MG_Util/Types.h>
#include <Config.h>
#include <MG_Pipe/MGPipe.h>
#include <MG_State/GLState/ProgramState/ProgramObject.h>
#include <MG_State/GLState/RenderbufferState/RenderbufferObject.h>
#include <MG_State/GLState/SamplerState/SamplerObject.h>
#include <MG_State/GLState/StateObjectDeathNotice.h>
#include <MG_State/GLState/TextureState/TextureObject2D.h>
#include <MG_State/GLState/VertexArrayState/VertexArrayObject.h>
#include <csignal>
#include <chrono>
#include <limits>
#include <set>
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#if MOBILEGL_PIPE_PUSH
// PipeApply.h does not guard itself (its push-only property comes from the root
// CMakeLists), so it is included from inside this arm and nowhere else.
#include <MG_Pipe/PipeApply.h>
#include <MG_State/GLState/BufferState/BufferObject.h>

namespace {
    // Which arm of the twin table this binary runs on.
    //
    // SanityTest never calls MG_ConfigLoader::Init, so MG_Config::Features keeps its static
    // defaults - and Features.PipePush's static default is 0 ("pull everything", Config.h),
    // which is the value a PULL build ships. Without this the D13 "must not break" cases
    // (the scratch-FBO scrub, the three context-generation guards on the texture /
    // framebuffer / renderbuffer twins, the sampled-set staleness walk and the whole-registry
    // ScopedDirectGLESTextureBindings fixture) exercised the legacy UnorderedMap arm in EVERY
    // build directory - so a push build's 82 sanity cases said nothing about the code this
    // package actually changed.
    //
    // The default here is therefore ConfigLoader's own push-build default
    // (kMGPipeSubsystemsMigratedAtP2, ConfigLoader.cpp), i.e. this binary runs the arm that
    // SHIPS in the build it was compiled for: legacy in build-linux (where the handle arm is
    // not compiled at all and every DirectGLESSlotTable case skips), handles in build-push and
    // build-verify. MOBILEGL_PIPE_PUSH in the environment overrides it with the same
    // decimal/0x contract ConfigLoader.cpp:172-192 gives it, so `MOBILEGL_PIPE_PUSH=0
    // ctest -R Sanity` is the legacy-arm run of the same binary and the A/B is one env var.
    //
    // It runs as a gtest Environment rather than a static initializer on purpose:
    // MG_Config::Features has a String member, so it is dynamically initialised, and writing
    // to it from another TU's static initializer would be an initialisation-order race.
    // SetUp() runs inside RUN_ALL_TESTS, long after every static initializer, and before the
    // first test - hence before anything can latch EsprytSlotTablesEnabled().
    class EsprytSlotArmEnvironment final : public ::testing::Environment {
    public:
        void SetUp() override {
            MobileGL::Uint64 bits = MobileGL::MG_Pipe::kMGPipeSubsystemsMigratedAtP2;
            const char* knob = std::getenv("MOBILEGL_PIPE_PUSH");
            if (knob != nullptr && *knob != '\0') {
                bits = std::strtoull(knob, nullptr, 0);
            }
            MobileGL::MG_Config::Features.PipePush = bits;
        }
    };

    const ::testing::Environment* g_esprytSlotArmEnvironment =
        ::testing::AddGlobalTestEnvironment(new EsprytSlotArmEnvironment());
} // namespace
#endif // MOBILEGL_PIPE_PUSH

namespace {
    class DynamicParameterBackend final : public MobileGL::MG_Backend::BackendObject {
    public:
        // `type` defaults to Unknown, which is what every existing case wanted: a limits-only
        // double with no backend identity. A case that captures a CompileEnv from it and then
        // compares the result against glGetIntegerv has to pass a REAL type, because
        // CompileEnv::HasBackend() is what BuildTBuiltInResource bounds gl_MaxVertexAttribs by
        // while the getter bounds it by "a backend object exists" - two spellings of the same
        // thing in production, and only in production.
        explicit DynamicParameterBackend(MobileGL::MG_Backend::DynamicBackendParameters params,
                                         MobileGL::BackendType type = MobileGL::BackendType::Unknown):
            m_params(params), m_type(type) {}

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
        MobileGL::BackendType GetBackendType() const override { return m_type; }

    private:
        MobileGL::MG_Backend::DynamicBackendParameters m_params;
        MobileGL::BackendType m_type = MobileGL::BackendType::Unknown;
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

// Two strings that name capabilities MobileGL has always had, and that were missing from the
// advertised list for as long as it existed.
//
// GL_ARB_uniform_buffer_object is the one with teeth: applications gate the ENTRY POINTS on the
// string rather than on the context version. KHR-GL4x.transform_feedback.draw_xfb_instanced_test
// resolves glGetUniformBlockIndex / glUniformBlockBinding only inside `if (is_arb_ubo)`, then
// calls them unconditionally because the context claims >= 4.2 - so a missing string turned into
// a call through a null pointer and took the whole process down with SIGSEGV. Withdrawing it
// again would restore that crash on both backends.
//
// GL_ARB_stencil_texturing is what makes DEPTH_STENCIL_TEXTURE_MODE = GL_STENCIL_INDEX reachable
// at all before GL 4.3, which is the whole of KHR-GL3x.packed_depth_stencil.stencil_texturing.
TEST(DirectGLESSanity, AdvertisesUniformBufferObjectAndStencilTexturing) {
    MobileGL::MG_Backend::DirectGLES::BackendObject_DirectGLES backend;
    const auto& extensions = backend.GetRendererInfo().RendererGLInfo.Extensions;

    EXPECT_NE(std::find(extensions.begin(), extensions.end(), MobileGL::E_GL_ARB_uniform_buffer_object),
              extensions.end());
    EXPECT_NE(std::find(extensions.begin(), extensions.end(), MobileGL::E_GL_ARB_stencil_texturing),
              extensions.end());
}

TEST(DirectVulkanSanity, AdvertisesUniformBufferObjectAndStencilTexturing) {
    MobileGL::MG_Backend::DirectVulkan::BackendObject_DirectVulkan backend;
    const auto& extensions = backend.GetRendererInfo().RendererGLInfo.Extensions;

    EXPECT_NE(std::find(extensions.begin(), extensions.end(), MobileGL::E_GL_ARB_uniform_buffer_object),
              extensions.end());
    EXPECT_NE(std::find(extensions.begin(), extensions.end(), MobileGL::E_GL_ARB_stencil_texturing),
              extensions.end());
}

// Voxy only ever needed the extensions, which stay advertised whatever the version is; the version
// assertion just pins what the backend really reports, now that V_OpenGL40 is in the list.
TEST(DirectGLESSanity, AdvertisesVoxyRequiredRenderingExtensions) {
    MobileGL::MG_Backend::DirectGLES::BackendObject_DirectGLES backend;
    const auto& rendererInfo = backend.GetRendererInfo().RendererGLInfo;
    const auto& extensions = rendererInfo.Extensions;

    EXPECT_EQ(rendererInfo.TargetGLVersion.Major, 4);
    EXPECT_EQ(rendererInfo.TargetGLVersion.Minor, 6);
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

// A multisample texture is fetched, never filtered, so the mip-chain completeness rules never
// apply to it (GL 4.6 core 8.17). It has exactly one level and MIN_FILTER's initial value is
// NEAREST_MIPMAP_LINEAR, so asking those rules anyway calls EVERY multisample texture incomplete
// - and both backends express "incomplete" as "leave the native target unbound", which makes the
// shader's sampler2DMS read zero from a texture that was written correctly.
//
// That is KHR-GL43.compute_shader.resource-texture: it clears its 2DMS texture to 123.0 through
// an FBO (which succeeds - the ES clear is issued on a COMPLETE 4-sample framebuffer with no
// error) and then fails at the first sampler2DMS element because the texture was never bound.
TEST(DirectGLESSanity, BindsAMultisampleTextureDespiteTheDefaultMipmapFilter) {
    using namespace MobileGL;
    namespace DirectGLES = MG_Backend::DirectGLES;

    ScopedDirectGLESTextureBindings state;

    GLuint frontendTexture = 0;
    MG_Impl::GLImpl::GenTextures(1, &frontendTexture);
    ASSERT_NE(frontendTexture, 0u);
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D_MULTISAMPLE, frontendTexture);
    const auto& textureObject = MG_State::pGLContext->GetTextureUnitObject(0)
                                    .GetBindingSlot(TextureTarget::Texture2DMultisample)
                                    .GetBoundObject();
    ASSERT_NE(textureObject, nullptr);

    textureObject->SetInternalFormat(TextureInternalFormat::RGBA8);
    textureObject->SetSamples(4);
    textureObject->SetFixedSampleLocations(false);
    // One level, 4x4 - the shape glTexImage2DMultisample produces, and a size whose mip chain
    // would need three levels if the filter rules were (wrongly) applied.
    MG_State::GLState::AsMipmapTexture(textureObject.get())
        ->AllocateStorage(TextureUploadTarget::Texture2DMultisample, 0, {{4, 4, 1}, 4});

    // The precondition that used to poison it, asserted rather than assumed: the texture's own
    // sampler still reports a mipmapping filter, because GL's initial MIN_FILTER is
    // NEAREST_MIPMAP_LINEAR and a multisample texture has no way (and no reason) to change it.
    // If a future default made this None the test would pass without covering anything.
    const auto& sampler = textureObject->GetSamplerObject();
    ASSERT_NE(sampler, nullptr);
    ASSERT_NE(sampler->GetMipmapMode(), SamplerMipmapMode::None)
        << "fixture is stale: the default sampler no longer asks for mipmapping, so this test "
           "would not exercise the multisample guard";

    EXPECT_FALSE(MG_State::GLState::SamplesAsIncompleteTexture(textureObject.get(), sampler.get()))
        << "a multisample texture is never filter-incomplete";

    auto& backendTexture = DirectGLES::TextureImpl::g_backendTextureObjects.GetOrCreate(textureObject);
    backendTexture = MakeShared<DirectGLES::TextureImpl::BackendTextureObject>();
    const GLuint backendTextureId = backendTexture->GetBackendTextureId();

    // The symptom itself: the per-unit walk has to actually bind it. BindCurrentTextures() is
    // the per-draw walk - it reads GetProgramForDraw - so the block it reads is the one a draw
    // fills; without saying so, its first read is Fatal{UnmigratedPipeInput} in a push build.
    MG_Test::ScopedPipeVerb draw(MG_Pipe::MGPipeVerb::DrawArrays);
    DirectGLES::BindCurrentTextures();
    ASSERT_EQ(state.bindCalls.size(), 1u)
        << "the multisample texture was not bound; every texelFetch against it reads zero";
    EXPECT_EQ(state.bindCalls[0].target, GL_TEXTURE_2D_MULTISAMPLE);
    EXPECT_EQ(state.bindCalls[0].texture, backendTextureId);
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

    // A texture with no image is incomplete, and an incomplete texture samples as (0, 0, 0, 1) -
    // which DirectGLES expresses by leaving the native target unbound (see "sample a
    // mipmap-incomplete texture as black"). This test is about the bind-0 clear, so the texture
    // has to be complete enough to get bound in the first place: a format plus a level 0. At 1x1
    // that single level is the whole mip chain, so it stays complete under any filter. The state
    // is set directly rather than through glTexImage2D because the mock GLES table below wires
    // only the binding entry points, not the upload path.
    frontendTextureObject->SetInternalFormat(TextureInternalFormat::RGBA8);
    MG_State::GLState::AsMipmapTexture(frontendTextureObject.get())
        ->AllocateStorage(TextureUploadTarget::Texture2D, 0, {{1, 1, 1}, 4});
    ASSERT_FALSE(MG_State::GLState::SamplesAsIncompleteTexture(
        frontendTextureObject.get(), frontendTextureObject->GetSamplerObject().get()));

    auto& backendTexture = DirectGLES::TextureImpl::g_backendTextureObjects.GetOrCreate(frontendTextureObject);
    backendTexture = MakeShared<DirectGLES::TextureImpl::BackendTextureObject>();
    const GLuint backendTextureId = backendTexture->GetBackendTextureId();

    // Each walk below is the texture half of one draw, so each gets its own verb (the second
    // and third stand after frontend state moved, exactly as a second entry point's fill would).
    MG_Test::ScopedPipeVerb draw(MG_Pipe::MGPipeVerb::DrawArrays);
    DirectGLES::BindCurrentTextures();
    ASSERT_EQ(state.bindCalls.size(), 1u);
    EXPECT_EQ(state.bindCalls[0].target, GL_TEXTURE_2D);
    EXPECT_EQ(state.bindCalls[0].texture, backendTextureId);

    // The default 1D slot maps to the same native ES target as 2D. It must not clear and force a
    // redundant rebind while the real 2D frontend object remains current.
    draw.Renew();
    DirectGLES::BindCurrentTextures();
    EXPECT_EQ(state.bindCalls.size(), 1u);

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, 0);
    ASSERT_TRUE(MG_State::GLState::IsUndefinedDefaultTexture(
        MG_State::pGLContext->GetTextureUnitObject(0)
            .GetBindingSlot(TextureTarget::Texture2D)
            .GetBoundObject()
            .get()));

    draw.Renew();
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
    // The indirect lowering reads its baseInstance through a storage block declared in the
    // VERTEX stage, which is optional in both APIs and which the GLESCapabilities default
    // (0, the spec minimum) therefore denies. This suite is pinning the shape of that
    // lowering, so it has to describe a driver that can actually have it - see
    // VertexStageStorageBlockUsable and the BaseInstanceInjectionGate suite for the
    // zero case.
    caps.MaxVertexShaderStorageBlocks = 1;

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
    // One-based word index: zero is the "not an indirect draw" sentinel because that is
    // the value a GLSL uniform starts at and no draw path writes it before the first draw.
    EXPECT_NE(rewritten.find("#define mg_ZeroBasedInstanceID (gl_InstanceID - ((mg_BaseInstanceWordIndex > 0) ? "
                             "int(mg_indirectWords[uint(mg_BaseInstanceWordIndex - 1)]) : 0))"),
              MobileGL::String::npos);
    EXPECT_NE(rewritten.find(
                  "layout(std430, binding = 12) readonly buffer mg_IndirectParams { highp uint mg_indirectWords[]; };"),
              MobileGL::String::npos);
    // The one inside the #define machinery must be the only surviving gl_InstanceID.
    EXPECT_EQ(CountOccurrences(rewritten, "gl_InstanceID"), 1u);
}

// The sentinel itself, on the builtin it exists for. A zero-based index with a
// negative "off" value made every NON-indirect draw of such a program read
// mg_indirectWords[0] out of a storage buffer nothing had bound - the uniform starts
// at zero and no non-indirect draw path writes it - which is where the CTS
// shader_draw_parameters cases lost their geometry on Adreno. Pinned as text because
// this contract lives in two places at once: the generated ESSL below and the +1 that
// BackendProgramObjectImpl::SetBaseInstanceWordIndex applies.
TEST(DirectGLESSanity, TheIndirectWordIndexIsOneBasedSoItsUnwrittenValueMeansNotIndirect) {
    const ScopedGLESCapabilitiesOverride capsGuard;
    auto& caps = MobileGL::MG_Backend::DirectGLES::g_GLESCapabilities;
    caps.IndirectDrawInstanceIdIncludesBaseInstance = false;
    caps.MaxShaderStorageBufferBindings = 13;
    // See RebasesInstanceIdWhenIndirectDrawsLeakBaseInstance: without a vertex-stage
    // storage block there is no word index to be one-based about.
    caps.MaxVertexShaderStorageBlocks = 1;

    const MobileGL::String source = R"(#version 310 es
highp int mg_BaseInstanceLowered;
void main() {
    gl_Position = vec4(float(mg_BaseInstanceLowered));
}
)";

    const auto rewritten = MobileGL::MG_Backend::DirectGLES::PromoteDrawParameterGlobalsToUniforms(
        source, GL_VERTEX_SHADER);

    EXPECT_NE(rewritten.find("#define mg_BaseInstanceLowered ((mg_BaseInstanceWordIndex > 0) ? "
                             "int(mg_indirectWords[uint(mg_BaseInstanceWordIndex - 1)]) : mg_BaseInstance)"),
              MobileGL::String::npos)
        << rewritten;
    // A zero-based form would spell either of these; neither may survive.
    EXPECT_EQ(rewritten.find("mg_BaseInstanceWordIndex >= 0"), MobileGL::String::npos);
    EXPECT_EQ(rewritten.find("uint(mg_BaseInstanceWordIndex)"), MobileGL::String::npos);
}

TEST(DirectGLESSanity, KeepsInstanceIdWhenIndirectDrawsAreConforming) {
    const ScopedGLESCapabilitiesOverride capsGuard;
    auto& caps = MobileGL::MG_Backend::DirectGLES::g_GLESCapabilities;
    caps.IndirectDrawInstanceIdIncludesBaseInstance = false;
    caps.MaxShaderStorageBufferBindings = 13;
    // Set explicitly even though the assertions below would also hold on the degraded path:
    // this case is about a CONFORMING driver leaving gl_InstanceID alone, and it would be a
    // silent weakening for it to be exercising the no-storage-block fallback instead.
    caps.MaxVertexShaderStorageBlocks = 1;

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
    // The indirect view is present on this driver, so the fallback must NOT have fired.
    EXPECT_NE(rewritten.find("buffer mg_IndirectParams"), MobileGL::String::npos);
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

// A sampled view of a combined depth/stencil image may name exactly one aspect, and
// GL_DEPTH_STENCIL_TEXTURE_MODE picks which - the whole of GL_ARB_stencil_texturing on this
// backend. Depth remains the answer for everything that does not ask for stencil, including
// depth-only images asked for the stencil aspect they do not have.
TEST(DirectVulkanSanity, SampledViewAspectFollowsDepthStencilTextureMode) {
    using MobileGL::MG_Backend::DirectVulkan::VkTextureManager;
    constexpr VkImageAspectFlags kPacked = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;

    EXPECT_EQ(VkTextureManager::ResolveSampledImageViewAspectMask(kPacked, GL_DEPTH_COMPONENT),
              static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_DEPTH_BIT));
    EXPECT_EQ(VkTextureManager::ResolveSampledImageViewAspectMask(kPacked, GL_STENCIL_INDEX),
              static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_STENCIL_BIT));
    // The default argument is the pre-existing behaviour, for the call sites with no texture.
    EXPECT_EQ(VkTextureManager::ResolveSampledImageViewAspectMask(kPacked),
              static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_DEPTH_BIT));

    // Single-aspect images ignore the mode: there is only one aspect to name.
    EXPECT_EQ(VkTextureManager::ResolveSampledImageViewAspectMask(VK_IMAGE_ASPECT_DEPTH_BIT, GL_STENCIL_INDEX),
              static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_DEPTH_BIT));
    EXPECT_EQ(VkTextureManager::ResolveSampledImageViewAspectMask(VK_IMAGE_ASPECT_STENCIL_BIT, GL_DEPTH_COMPONENT),
              static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_STENCIL_BIT));
    EXPECT_EQ(VkTextureManager::ResolveSampledImageViewAspectMask(VK_IMAGE_ASPECT_COLOR_BIT, GL_STENCIL_INDEX),
              static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT));
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

// See the DirectGLES twin above: the target version follows the advertised list, the extensions are
// what matter.
TEST(DirectVulkanSanity, AdvertisesVoxyRequiredRenderingExtensions) {
    MobileGL::MG_Backend::DirectVulkan::BackendObject_DirectVulkan backend;
    const auto& rendererInfo = backend.GetRendererInfo().RendererGLInfo;
    const auto& extensions = rendererInfo.Extensions;

    EXPECT_EQ(rendererInfo.TargetGLVersion.Major, 4);
    EXPECT_EQ(rendererInfo.TargetGLVersion.Minor, 6);
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

TEST(DirectVulkanSanity, GatesPerStageImageUniformLimitsOnPhysicalDeviceFeatures) {
    using namespace MobileGL;

    MG_Backend::DirectVulkan::BackendObject_DirectVulkan backend;
    MG_External::VulkanCapabilities caps;
    caps.MaxImageUnits = 12;
    caps.MaxCombinedImageUniforms = 10;
    caps.MaxComputeImageUniforms = 9;
    caps.SupportsVertexPipelineStoresAndAtomics = true;
    caps.SupportsFragmentStoresAndAtomics = true;
    caps.SupportsGeometryShader = false;
    backend.ApplyVulkanCapabilitiesForTesting(caps);

    const auto& withoutGeometry = backend.GetDynamicParameters();
    EXPECT_EQ(withoutGeometry.MaxVertexImageUniforms, 10);
    EXPECT_EQ(withoutGeometry.MaxGeometryImageUniforms, 0);
    EXPECT_EQ(withoutGeometry.MaxFragmentImageUniforms, 10);
    EXPECT_EQ(withoutGeometry.MaxComputeImageUniforms, 9);

    caps.SupportsGeometryShader = true;
    backend.ApplyVulkanCapabilitiesForTesting(caps);
    EXPECT_EQ(backend.GetDynamicParameters().MaxGeometryImageUniforms, 10);

    caps.SupportsVertexPipelineStoresAndAtomics = false;
    caps.SupportsFragmentStoresAndAtomics = false;
    backend.ApplyVulkanCapabilitiesForTesting(caps);
    EXPECT_EQ(backend.GetDynamicParameters().MaxVertexImageUniforms, 0);
    EXPECT_EQ(backend.GetDynamicParameters().MaxGeometryImageUniforms, 0);
    EXPECT_EQ(backend.GetDynamicParameters().MaxFragmentImageUniforms, 0);
    EXPECT_EQ(backend.GetDynamicParameters().MaxComputeImageUniforms, 9);
}

TEST(DirectGLESSanity, PreservesHostPerStageImageUniformLimits) {
    using namespace MobileGL;

    MG_Backend::DirectGLES::BackendObject_DirectGLES backend;
    MG_External::GLESCapabilities caps;
    caps.MaxImageUnits = 8;
    caps.MaxCombinedImageUniforms = 16;
    caps.MaxVertexImageUniforms = 2;
    caps.MaxGeometryImageUniforms = 3;
    caps.MaxFragmentImageUniforms = 4;
    caps.MaxComputeImageUniforms = 5;
    backend.ApplyGLESCapabilitiesForTesting(caps);

    const auto& params = backend.GetDynamicParameters();
    EXPECT_EQ(params.MaxVertexImageUniforms, 2);
    EXPECT_EQ(params.MaxGeometryImageUniforms, 3);
    EXPECT_EQ(params.MaxFragmentImageUniforms, 4);
    EXPECT_EQ(params.MaxComputeImageUniforms, 5);
}

// maxClipDistances is a LIMIT every Vulkan device reports; declaring ClipDistance in a module
// needs the shaderClipDistance FEATURE, which is separate and which VulkanRenderer enables only
// where the physical device has it. Forwarding the limit without the feature advertises eight
// clip planes no shader may use - the same shape as the image-uniform limits above, and the same
// shape as the GL_EXT_clip_cull_distance lie on DirectGLES. Not a blanket zero: a device WITH the
// feature keeps its real number.
TEST(DirectVulkanSanity, GatesClipDistancesOnTheShaderClipDistanceFeature) {
    using namespace MobileGL;

    MG_Backend::DirectVulkan::BackendObject_DirectVulkan backend;
    MG_External::VulkanCapabilities caps;
    caps.MaxClipDistances = 8;

    caps.SupportsShaderClipDistance = false;
    backend.ApplyVulkanCapabilitiesForTesting(caps);
    EXPECT_EQ(backend.GetDynamicParameters().MaxClipDistances, 0);

    caps.SupportsShaderClipDistance = true;
    backend.ApplyVulkanCapabilitiesForTesting(caps);
    EXPECT_EQ(backend.GetDynamicParameters().MaxClipDistances, 8);
}

// The cull half of the same contract. shaderCullDistance is a SEPARATE feature from
// shaderClipDistance - VulkanRenderer enables each independently - so it gets its own gate, and
// the combined limit is gated on either being present because GL 4.6 core 11.1.3.10 makes it at
// least as large as both halves. These three used to be literal 8s inside BuildTBuiltInResource
// with no device consulted at all, which let glslang accept a gl_CullDistance write that then
// discarded every primitive it touched.
TEST(DirectVulkanSanity, GatesCullDistancesOnTheShaderCullDistanceFeature) {
    using namespace MobileGL;

    MG_Backend::DirectVulkan::BackendObject_DirectVulkan backend;
    MG_External::VulkanCapabilities caps;
    caps.MaxClipDistances = 8;
    caps.MaxCullDistances = 8;
    caps.MaxCombinedClipAndCullDistances = 8;

    caps.SupportsShaderClipDistance = false;
    caps.SupportsShaderCullDistance = false;
    backend.ApplyVulkanCapabilitiesForTesting(caps);
    EXPECT_EQ(backend.GetDynamicParameters().MaxCullDistances, 0);
    EXPECT_EQ(backend.GetDynamicParameters().MaxCombinedClipAndCullDistances, 0);

    // Clip only: cull stays zero, and the combined limit still describes the clip capacity.
    caps.SupportsShaderClipDistance = true;
    backend.ApplyVulkanCapabilitiesForTesting(caps);
    EXPECT_EQ(backend.GetDynamicParameters().MaxCullDistances, 0);
    EXPECT_EQ(backend.GetDynamicParameters().MaxCombinedClipAndCullDistances, 8);

    caps.SupportsShaderCullDistance = true;
    backend.ApplyVulkanCapabilitiesForTesting(caps);
    EXPECT_EQ(backend.GetDynamicParameters().MaxCullDistances, 8);
    EXPECT_EQ(backend.GetDynamicParameters().MaxCombinedClipAndCullDistances, 8);
}

// DirectGLES reaches clip AND cull distances only through GL_EXT_clip_cull_distance, so the
// loader leaves all three at zero without it and the backend forwards that verbatim. Zero is the
// answer that stops a gl_CullDistance shader from reaching an ESSL compiler that would reject it.
TEST(DirectGLESSanity, ForwardsTheProbedClipAndCullDistanceLimits) {
    using namespace MobileGL;

    MG_Backend::DirectGLES::BackendObject_DirectGLES backend;
    MG_External::GLESCapabilities caps;
    backend.ApplyGLESCapabilitiesForTesting(caps);
    EXPECT_EQ(backend.GetDynamicParameters().MaxClipDistances, 0);
    EXPECT_EQ(backend.GetDynamicParameters().MaxCullDistances, 0);
    EXPECT_EQ(backend.GetDynamicParameters().MaxCombinedClipAndCullDistances, 0);

    caps.SupportsClipDistance = true;
    caps.MaxClipDistances = 8;
    caps.MaxCullDistances = 8;
    caps.MaxCombinedClipAndCullDistances = 8;
    backend.ApplyGLESCapabilitiesForTesting(caps);
    EXPECT_EQ(backend.GetDynamicParameters().MaxClipDistances, 8);
    EXPECT_EQ(backend.GetDynamicParameters().MaxCullDistances, 8);
    EXPECT_EQ(backend.GetDynamicParameters().MaxCombinedClipAndCullDistances, 8);
}

// GL_LAYER_PROVOKING_VERTEX / GL_VIEWPORT_INDEX_PROVOKING_VERTEX were a hard-coded
// GL_LAST_VERTEX_CONVENTION for both backends, derived from nothing, and wrong on both test
// devices in opposite directions. DirectGLES now forwards what its loader resolved; DirectVulkan
// reports GL_UNDEFINED_VERTEX, which GL 4.6 table 23.65 permits and which is what the backend
// honestly implements - the provoking mode is chosen per pipeline out of VK_EXT_provoking_vertex,
// provokingVertexModePerPipeline and the topology.
TEST(ProvokingVertexConventions, EachBackendReportsWhatItActuallyPins) {
    using namespace MobileGL;

    MG_Backend::DirectGLES::BackendObject_DirectGLES glesBackend;
    MG_External::GLESCapabilities glesCaps;
    glesCaps.LayerProvokingVertex = GL_FIRST_VERTEX_CONVENTION;
    glesCaps.ViewportIndexProvokingVertex = GL_UNDEFINED_VERTEX;
    glesBackend.ApplyGLESCapabilitiesForTesting(glesCaps);
    EXPECT_EQ(glesBackend.GetDynamicParameters().LayerProvokingVertex,
              static_cast<GLenum>(GL_FIRST_VERTEX_CONVENTION));
    EXPECT_EQ(glesBackend.GetDynamicParameters().ViewportIndexProvokingVertex,
              static_cast<GLenum>(GL_UNDEFINED_VERTEX));

    MG_Backend::DirectVulkan::BackendObject_DirectVulkan vkBackend;
    MG_External::VulkanCapabilities vkCaps;
    vkBackend.ApplyVulkanCapabilitiesForTesting(vkCaps);
    EXPECT_EQ(vkBackend.GetDynamicParameters().LayerProvokingVertex, static_cast<GLenum>(GL_UNDEFINED_VERTEX));
    EXPECT_EQ(vkBackend.GetDynamicParameters().ViewportIndexProvokingVertex,
              static_cast<GLenum>(GL_UNDEFINED_VERTEX));
}

TEST(FragmentInterpolationCapabilities, PlumbsGLESAndBothVulkanPropertyPaths) {
    using namespace MobileGL;

    MG_External::GLESCapabilities glesCaps;
    glesCaps.MinFragmentInterpolationOffset = -0.75f;
    glesCaps.MaxFragmentInterpolationOffset = 0.625f;
    glesCaps.FragmentInterpolationOffsetBits = 6;
    MG_Backend::DirectGLES::BackendObject_DirectGLES glesBackend;
    glesBackend.ApplyGLESCapabilitiesForTesting(glesCaps);
    EXPECT_FLOAT_EQ(glesBackend.GetDynamicParameters().MinFragmentInterpolationOffset, -0.75f);
    EXPECT_FLOAT_EQ(glesBackend.GetDynamicParameters().MaxFragmentInterpolationOffset, 0.625f);
    EXPECT_EQ(glesBackend.GetDynamicParameters().FragmentInterpolationOffsetBits, 6);

    VkPhysicalDeviceProperties properties{};
    // A common Vulkan limit pair: max is one representable 4-bit step below 0.5.
    properties.limits.minInterpolationOffset = -0.5f;
    properties.limits.maxInterpolationOffset = 0.4375f;
    properties.limits.subPixelInterpolationOffsetBits = 4;
    MG_External::VulkanCapabilities vkCaps;
    MG_Util::BackendLoader::FillInVulkanCapabilities(vkCaps, properties);
    EXPECT_FLOAT_EQ(vkCaps.MinFragmentInterpolationOffset, -0.5f);
    EXPECT_FLOAT_EQ(vkCaps.MaxFragmentInterpolationOffset, 0.4375f);
    EXPECT_EQ(vkCaps.FragmentInterpolationOffsetBits, 4);

    vkCaps.MinFragmentInterpolationOffset = -0.875f;
    vkCaps.MaxFragmentInterpolationOffset = 0.75f;
    vkCaps.FragmentInterpolationOffsetBits = 7;
    MG_Backend::DirectVulkan::BackendObject_DirectVulkan vkBackend;
    vkBackend.ApplyVulkanCapabilitiesForTesting(vkCaps);
    EXPECT_FLOAT_EQ(vkBackend.GetDynamicParameters().MinFragmentInterpolationOffset, -0.875f);
    EXPECT_FLOAT_EQ(vkBackend.GetDynamicParameters().MaxFragmentInterpolationOffset, 0.75f);
    EXPECT_EQ(vkBackend.GetDynamicParameters().FragmentInterpolationOffsetBits, 7);

    // Invalid/zero host data cannot under-advertise the OpenGL 4 minimums.
    MG_External::VulkanCapabilities invalidCaps;
    invalidCaps.MinFragmentInterpolationOffset = 0.0f;
    invalidCaps.MaxFragmentInterpolationOffset = 0.0f;
    invalidCaps.FragmentInterpolationOffsetBits = 0;
    vkBackend.ApplyVulkanCapabilitiesForTesting(invalidCaps);
    EXPECT_LE(vkBackend.GetDynamicParameters().MinFragmentInterpolationOffset, -0.5f);
    EXPECT_FLOAT_EQ(vkBackend.GetDynamicParameters().MaxFragmentInterpolationOffset, 0.4375f);
    EXPECT_EQ(vkBackend.GetDynamicParameters().FragmentInterpolationOffsetBits, 4);
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

TEST(DirectVulkanSanity, CapabilityRefreshInvalidatesTheCachedCompileEnvironment) {
    using namespace MobileGL;

    auto previousContext = Move(MG_State::pGLContext);
    auto previousBackend = Move(MG_Backend::pActiveBackendObject);
    MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();

    auto backend = MakeUnique<MG_Backend::DirectVulkan::BackendObject_DirectVulkan>();
    auto* backendPtr = backend.get();
    MG_Backend::pActiveBackendObject = Move(backend);

    const auto before = MG_State::pGLContext->GetCompileEnv();
    EXPECT_EQ(before->params.SubgroupSize, 0u);

    MG_External::VulkanCapabilities caps;
    caps.SupportsShaderSubgroup = true;
    caps.SubgroupSize = 8;
    caps.SubgroupSupportedStages = VK_SHADER_STAGE_COMPUTE_BIT;
    caps.SubgroupSupportedOperations = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT;
    backendPtr->ApplyVulkanCapabilitiesForTesting(caps);

    const auto after = MG_State::pGLContext->GetCompileEnv();
    EXPECT_NE(after.get(), before.get());
    EXPECT_NE(after->fingerprint, before->fingerprint);
    EXPECT_EQ(after->backend, BackendType::DirectVulkan);
    EXPECT_EQ(after->params.SubgroupSize, 8u);

    MG_Backend::pActiveBackendObject = Move(previousBackend);
    MG_State::pGLContext = Move(previousContext);
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

TEST(GetterSanity, ReportsFragmentInterpolationLimitsForFloatAndIntegerQueries) {
    using namespace MobileGL;

    auto previousContext = Move(MG_State::pGLContext);
    auto previousBackend = Move(MG_Backend::pActiveBackendObject);
    MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();

    MG_Backend::DynamicBackendParameters params;
    params.MinFragmentInterpolationOffset = -0.75f;
    params.MaxFragmentInterpolationOffset = 0.4375f;
    params.FragmentInterpolationOffsetBits = 6;
    MG_Backend::pActiveBackendObject = MakeUnique<DynamicParameterBackend>(params);

    GLfloat floatValue = 0.0f;
    MG_Impl::GLImpl::GetFloatv(GL_MIN_FRAGMENT_INTERPOLATION_OFFSET, &floatValue);
    EXPECT_FLOAT_EQ(floatValue, -0.75f);
    MG_Impl::GLImpl::GetFloatv(GL_MAX_FRAGMENT_INTERPOLATION_OFFSET, &floatValue);
    EXPECT_FLOAT_EQ(floatValue, 0.4375f);
    MG_Impl::GLImpl::GetFloatv(GL_FRAGMENT_INTERPOLATION_OFFSET_BITS, &floatValue);
    EXPECT_FLOAT_EQ(floatValue, 6.0f);

    GLint intValue = 0;
    MG_Impl::GLImpl::GetIntegerv(GL_MIN_FRAGMENT_INTERPOLATION_OFFSET, &intValue);
    EXPECT_EQ(intValue, -1);
    MG_Impl::GLImpl::GetIntegerv(GL_MAX_FRAGMENT_INTERPOLATION_OFFSET, &intValue);
    EXPECT_EQ(intValue, 0);
    MG_Impl::GLImpl::GetIntegerv(GL_FRAGMENT_INTERPOLATION_OFFSET_BITS, &intValue);
    EXPECT_EQ(intValue, 6);

    GLboolean boolValue = GL_FALSE;
    MG_Impl::GLImpl::GetBooleanv(GL_MIN_FRAGMENT_INTERPOLATION_OFFSET, &boolValue);
    EXPECT_EQ(boolValue, GL_TRUE);
    MG_Impl::GLImpl::GetBooleanv(GL_MAX_FRAGMENT_INTERPOLATION_OFFSET, &boolValue);
    EXPECT_EQ(boolValue, GL_TRUE);
    MG_Impl::GLImpl::GetBooleanv(GL_FRAGMENT_INTERPOLATION_OFFSET_BITS, &boolValue);
    EXPECT_EQ(boolValue, GL_TRUE);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Backend::pActiveBackendObject = Move(previousBackend);
    MG_State::pGLContext = Move(previousContext);
}

// GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT used to be answered with the UNIFORM buffer
// alignment. The two are separate limits and the storage one is the larger on real hardware
// (Adreno 830: 32 uniform, 64 storage), so the substitution under-reported it - and an
// under-reported alignment is silent all the way down: the frontend validator accepts the
// offset, the ES driver accepts the glBindBufferRange too without raising an error, and the
// shader's stores land at an address the application never bound. The two values are
// deliberately different here so a query that reads the wrong field cannot coincide with the
// right answer.
TEST(GetterSanity, StorageAndUniformBufferOffsetAlignmentsAreSeparateLimits) {
    using namespace MobileGL;

    auto previousContext = Move(MG_State::pGLContext);
    auto previousBackend = Move(MG_Backend::pActiveBackendObject);
    MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();

    MG_Backend::DynamicBackendParameters params;
    params.UniformBufferOffsetAlignment = 32;
    params.ShaderStorageBufferOffsetAlignment = 64;
    MG_Backend::pActiveBackendObject = MakeUnique<DynamicParameterBackend>(params);

    GLint uniformAlignment = 0;
    MG_Impl::GLImpl::GetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &uniformAlignment);
    EXPECT_EQ(uniformAlignment, 32);

    GLint storageAlignment = 0;
    MG_Impl::GLImpl::GetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &storageAlignment);
    EXPECT_EQ(storageAlignment, 64);

    // And the other way round, so the test fails on a getter that simply swapped the two fields.
    params.UniformBufferOffsetAlignment = 128;
    params.ShaderStorageBufferOffsetAlignment = 16;
    MG_Backend::pActiveBackendObject = MakeUnique<DynamicParameterBackend>(params);

    MG_Impl::GLImpl::GetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &uniformAlignment);
    EXPECT_EQ(uniformAlignment, 128);
    MG_Impl::GLImpl::GetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &storageAlignment);
    EXPECT_EQ(storageAlignment, 16);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Backend::pActiveBackendObject = Move(previousBackend);
    MG_State::pGLContext = Move(previousContext);
}

TEST(GetterSanity, PerStageImageUniformQueriesMatchShaderCompilerLimits) {
    using namespace MobileGL;

    MG_Backend::DynamicBackendParameters params;
    params.MaxImageUnits = 8;
    params.MaxCombinedImageUniforms = 8;
    params.MaxVertexImageUniforms = 1;
    params.MaxGeometryImageUniforms = 2;
    params.MaxFragmentImageUniforms = 3;
    params.MaxComputeImageUniforms = 4;
    MG_Backend::pActiveBackendObject = MakeUnique<DynamicParameterBackend>(params);

    GLint reported = -1;
    MG_Impl::GLImpl::GetIntegerv(GL_MAX_VERTEX_IMAGE_UNIFORMS, &reported);
    EXPECT_EQ(reported, 1);
    MG_Impl::GLImpl::GetIntegerv(GL_MAX_GEOMETRY_IMAGE_UNIFORMS, &reported);
    EXPECT_EQ(reported, 2);
    MG_Impl::GLImpl::GetIntegerv(GL_MAX_FRAGMENT_IMAGE_UNIFORMS, &reported);
    EXPECT_EQ(reported, 3);
    MG_Impl::GLImpl::GetIntegerv(GL_MAX_COMPUTE_IMAGE_UNIFORMS, &reported);
    EXPECT_EQ(reported, 4);

    const String vertexImageStore = R"(#version 430 core
layout(r32ui, binding = 0) uniform uimage2D targetImages[gl_MaxVertexImageUniforms];
void main() {
    imageStore(targetImages[0], ivec2(0), uvec4(1));
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
)";
    auto supported = MG_Util::ShaderTranspiler::ShaderCompiler::CompileShader({
        .shaderType = GL_VERTEX_SHADER,
        .sourceStr = vertexImageStore,
    });
    EXPECT_TRUE(supported) << (supported ? "" : supported.error().log);

    params.MaxVertexImageUniforms = 0;
    MG_Backend::pActiveBackendObject = MakeUnique<DynamicParameterBackend>(params);
    MG_Impl::GLImpl::GetIntegerv(GL_MAX_VERTEX_IMAGE_UNIFORMS, &reported);
    EXPECT_EQ(reported, 0);
    auto unsupported = MG_Util::ShaderTranspiler::ShaderCompiler::CompileShader({
        .shaderType = GL_VERTEX_SHADER,
        .sourceStr = vertexImageStore,
    });
    EXPECT_FALSE(unsupported);

    MG_Backend::pActiveBackendObject.reset();
}

// KHR-GL43.shader_atomic_counters.basic-glsl-built-in, .basic-buffer-bind and .basic-api-get.
// The atomic-counter limits used to live in two unreconciled tables - glslang compiled every
// shader against ONE binding while glGetIntegerv advertised thirty-six - and three of the enums
// had no case in the getter at all, so the query raised INVALID_ENUM and left the caller reading
// whatever was in its own stack slot.
TEST(GetterSanity, AtomicCounterQueriesMatchShaderCompilerLimits) {
    using namespace MobileGL;
    namespace Transpiler = MG_Util::ShaderTranspiler;

    auto previousContext = Move(MG_State::pGLContext);
    auto previousBackend = Move(MG_Backend::pActiveBackendObject);
    MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();
    MG_Backend::pActiveBackendObject = MakeUnique<DynamicParameterBackend>(MG_Backend::DynamicBackendParameters{});

    GLint reported = -1;
    MG_Impl::GLImpl::GetIntegerv(GL_MAX_ATOMIC_COUNTER_BUFFER_BINDINGS, &reported);
    EXPECT_EQ(reported, static_cast<GLint>(Transpiler::MAX_ATOMIC_COUNTER_BUFFER_BINDINGS));
    MG_Impl::GLImpl::GetIntegerv(GL_MAX_ATOMIC_COUNTER_BUFFER_SIZE, &reported);
    EXPECT_EQ(reported, static_cast<GLint>(Transpiler::MAX_ATOMIC_COUNTER_BUFFER_SIZE));
    for (const GLenum pname : {GL_MAX_COMBINED_ATOMIC_COUNTER_BUFFERS, GL_MAX_FRAGMENT_ATOMIC_COUNTER_BUFFERS,
                               GL_MAX_COMPUTE_ATOMIC_COUNTER_BUFFERS}) {
        reported = -1;
        MG_Impl::GLImpl::GetIntegerv(pname, &reported);
        EXPECT_EQ(reported, static_cast<GLint>(Transpiler::MAX_ATOMIC_COUNTER_BUFFERS_PER_STAGE))
            << "pname " << pname;
    }
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // glBindBufferBase sets the GENERIC binding point too (GL 4.6 6.1.1), and this is the one
    // indexed-buffer family whose non-indexed query had no case.
    reported = -1;
    MG_Impl::GLImpl::GetIntegerv(GL_ATOMIC_COUNTER_BUFFER_BINDING, &reported);
    EXPECT_EQ(reported, 0);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    GLuint buffer = 0;
    MG_Impl::GLImpl::GenBuffers(1, &buffer);
    MG_Impl::GLImpl::BindBuffer(GL_ATOMIC_COUNTER_BUFFER, buffer);
    MG_Impl::GLImpl::BufferData(GL_ATOMIC_COUNTER_BUFFER, 64, nullptr, GL_STATIC_DRAW);
    MG_Impl::GLImpl::BindBufferBase(GL_ATOMIC_COUNTER_BUFFER, 2, buffer);
    MG_Impl::GLImpl::GetIntegerv(GL_ATOMIC_COUNTER_BUFFER_BINDING, &reported);
    EXPECT_EQ(static_cast<GLuint>(reported), buffer);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // The advertised ceiling is also the one glBindBufferBase and the indexed getter enforce.
    // A limit nothing validates against is how these tables drifted apart in the first place:
    // the binding-point ARRAY is 36 deep, and it used to be that number an application saw.
    constexpr GLuint pastLastBinding = static_cast<GLuint>(Transpiler::MAX_ATOMIC_COUNTER_BUFFER_BINDINGS);
    MG_Impl::GLImpl::BindBufferBase(GL_ATOMIC_COUNTER_BUFFER, pastLastBinding, buffer);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), static_cast<GLenum>(GL_INVALID_VALUE));
    MG_Impl::GLImpl::GetIntegeri_v(GL_ATOMIC_COUNTER_BUFFER_BINDING, pastLastBinding, &reported);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), static_cast<GLenum>(GL_INVALID_VALUE));

    // ...and the shading language has to expand the same numbers. Each array is sized by a
    // built-in constant and indexed at its last element with a literal, so the stage only
    // compiles when that constant is at least what glGetIntegerv just reported - which it was
    // not while the resource table said one.
    const String lastBinding = std::to_string(Transpiler::MAX_ATOMIC_COUNTER_BUFFER_BINDINGS - 1);
    const String lastBuffer = std::to_string(Transpiler::MAX_ATOMIC_COUNTER_BUFFERS_PER_STAGE - 1);
    const String source = R"(#version 430 core
out vec4 color;
int mgBindings[gl_MaxAtomicCounterBindings];
int mgCombinedBuffers[gl_MaxCombinedAtomicCounterBuffers];
int mgFragmentBuffers[gl_MaxFragmentAtomicCounterBuffers];
layout(binding = )" + lastBinding + R"(, offset = 0) uniform atomic_uint mgCounter;
void main() {
    color = vec4(float(mgBindings[)" + lastBinding + R"(] + mgCombinedBuffers[)" + lastBuffer +
                         R"(] + mgFragmentBuffers[)" + lastBuffer + R"(] + int(atomicCounterIncrement(mgCounter))));
}
)";
    auto compiled = MG_Util::ShaderTranspiler::ShaderCompiler::CompileShader({
        .shaderType = GL_FRAGMENT_SHADER,
        .sourceStr = source,
    });
    EXPECT_TRUE(compiled) << (compiled ? "" : compiled.error().log);

    MG_Backend::pActiveBackendObject = Move(previousBackend);
    MG_State::pGLContext = Move(previousContext);
}

// KHR-GL43.compute_shader.max: the test queries every GL_MAX_COMPUTE_* value through the API and
// then makes a compute shader compare the matching gl_MaxCompute* constant against it. The two
// used to be independent tables and gl_MaxComputeWorkGroupSize.z disagreed - glslang compiled
// against a permissive 1024 while the context advertises the 64 the GL 4.6 minimum (and every ES
// driver) reports.
TEST(GetterSanity, ComputeWorkGroupQueriesMatchShaderCompilerLimits) {
    using namespace MobileGL;

    auto previousContext = Move(MG_State::pGLContext);
    auto previousBackend = Move(MG_Backend::pActiveBackendObject);
    MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();
    MG_Backend::pActiveBackendObject = MakeUnique<DynamicParameterBackend>(MG_Backend::DynamicBackendParameters{});

    GLint size[3] = {0, 0, 0};
    GLint count[3] = {0, 0, 0};
    for (GLuint index = 0; index < 3; ++index) {
        MG_Impl::GLImpl::GetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, index, &size[index]);
        MG_Impl::GLImpl::GetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, index, &count[index]);
    }
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    // The compile runs against a captured env, exactly as the pipeline's does. That is the whole
    // invariant: the env holds the same floored driver answer GetIntegeri_v just returned, so the
    // resource table and the query agree BY CONSTRUCTION rather than by two tables happening to
    // carry the same literals.
    const auto env = MG_Util::ShaderTranspiler::CaptureCompileEnv();
    for (GLuint index = 0; index < 3; ++index) {
        EXPECT_EQ(static_cast<GLint>(env->maxComputeWorkGroupSize[index]), size[index]) << "index " << index;
        EXPECT_EQ(static_cast<GLint>(env->maxComputeWorkGroupCount[index]), count[index]) << "index " << index;
    }

    // A negative array size is a compile error, so the stage only compiles when EVERY component
    // of both built-in constants equals what the query above reported. Two-sided by construction:
    // a resource table that is too permissive fails it exactly like one that is too tight.
    const String source = R"(#version 430 core
layout(local_size_x = 1) in;
const int mgAgree = (gl_MaxComputeWorkGroupSize == ivec3()" +
                         std::to_string(size[0]) + ", " + std::to_string(size[1]) + ", " +
                         std::to_string(size[2]) + R"() &&
                     gl_MaxComputeWorkGroupCount == ivec3()" +
                         std::to_string(count[0]) + ", " + std::to_string(count[1]) + ", " +
                         std::to_string(count[2]) + R"()) ? 1 : -1;
int mgProbe[mgAgree];
void main() {
    mgProbe[0] = 0;
}
)";
    auto compiled = MG_Util::ShaderTranspiler::ShaderCompiler::CompileShader({
        .shaderType = GL_COMPUTE_SHADER,
        .sourceStr = source,
        .env = env.get(),
    });
    EXPECT_TRUE(compiled) << (compiled ? "" : compiled.error().log);

    // The z ceiling is also what glslang checks a declared local_size_z against, so it has to
    // reject one invocation past the advertised limit and accept the limit itself.
    const String atLimit = "#version 430 core\nlayout(local_size_z = " + std::to_string(size[2]) +
                           ") in;\nvoid main() {}\n";
    const String pastLimit = "#version 430 core\nlayout(local_size_z = " + std::to_string(size[2] + 1) +
                             ") in;\nvoid main() {}\n";
    EXPECT_TRUE(MG_Util::ShaderTranspiler::ShaderCompiler::CompileShader({
        .shaderType = GL_COMPUTE_SHADER,
        .sourceStr = atLimit,
        .env = env.get(),
    }));
    EXPECT_FALSE(MG_Util::ShaderTranspiler::ShaderCompiler::CompileShader({
        .shaderType = GL_COMPUTE_SHADER,
        .sourceStr = pastLimit,
        .env = env.get(),
    }));

    MG_Backend::pActiveBackendObject = Move(previousBackend);
    MG_State::pGLContext = Move(previousContext);
}

// THE invariant every KHR-GL45.limits.* case checks, in one place. When the conformance table
// gives a limit both a glGetIntegerv pname and a GLSL built-in constant, it reads the query and
// then compiles a shader that writes the built-in into an SSBO and demands EXACT equality - so a
// limit answered from two unreconciled tables fails the SECOND half of the case, with a message
// about a number rather than about the two tables. Seven of them did: gl_MaxVertexAttribs said 64
// against a query of 32, gl_MaxDrawBuffers 32 against 8, gl_MaxCombinedTextureImageUnits 80
// against 96, gl_MaxVaryingComponents 60 against 64, gl_MaxCombinedShaderOutputResources 8
// against 29.
//
// KEEP THIS TABLE GROWING. Every pname added to GL_Getter that also has a gl_Max* built-in
// belongs here; that is what stops the next one from drifting.
TEST(GetterSanity, EveryLimitWithABuiltinAgreesWithItsQuery) {
    using namespace MobileGL;

    auto previousContext = Move(MG_State::pGLContext);
    auto previousBackend = Move(MG_Backend::pActiveBackendObject);
    MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();
    MG_Backend::pActiveBackendObject =
        MakeUnique<DynamicParameterBackend>(MG_Backend::DynamicBackendParameters{}, BackendType::DirectGLES);

    struct LimitPair {
        GLenum pname;
        const char* builtin;
    };
    const LimitPair pairs[] = {
        {GL_MAX_VERTEX_ATTRIBS, "gl_MaxVertexAttribs"},
        {GL_MAX_VERTEX_UNIFORM_COMPONENTS, "gl_MaxVertexUniformComponents"},
        {GL_MAX_VERTEX_UNIFORM_VECTORS, "gl_MaxVertexUniformVectors"},
        {GL_MAX_VERTEX_OUTPUT_COMPONENTS, "gl_MaxVertexOutputComponents"},
        {GL_MAX_VARYING_COMPONENTS, "gl_MaxVaryingComponents"},
        {GL_MAX_VARYING_VECTORS, "gl_MaxVaryingVectors"},
        {GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, "gl_MaxVertexTextureImageUnits"},
        {GL_MAX_TEXTURE_IMAGE_UNITS, "gl_MaxTextureImageUnits"},
        {GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, "gl_MaxCombinedTextureImageUnits"},
        {GL_MAX_FRAGMENT_UNIFORM_COMPONENTS, "gl_MaxFragmentUniformComponents"},
        {GL_MAX_FRAGMENT_UNIFORM_VECTORS, "gl_MaxFragmentUniformVectors"},
        {GL_MAX_FRAGMENT_INPUT_COMPONENTS, "gl_MaxFragmentInputComponents"},
        {GL_MAX_DRAW_BUFFERS, "gl_MaxDrawBuffers"},
        {GL_MAX_IMAGE_UNITS, "gl_MaxImageUnits"},
        // The SAME token (0x8F39) under two spellings, and the two glslang fields behind them
        // must therefore carry the same value.
        {GL_MAX_COMBINED_IMAGE_UNITS_AND_FRAGMENT_OUTPUTS, "gl_MaxCombinedImageUnitsAndFragmentOutputs"},
        {GL_MAX_COMBINED_SHADER_OUTPUT_RESOURCES, "gl_MaxCombinedShaderOutputResources"},
        {GL_MAX_CLIP_DISTANCES, "gl_MaxClipDistances"},
        {GL_MAX_CULL_DISTANCES, "gl_MaxCullDistances"},
        {GL_MAX_COMBINED_CLIP_AND_CULL_DISTANCES, "gl_MaxCombinedClipAndCullDistances"},
        {GL_MAX_SAMPLES, "gl_MaxSamples"},
        {GL_MIN_PROGRAM_TEXEL_OFFSET, "gl_MinProgramTexelOffset"},
        {GL_MAX_PROGRAM_TEXEL_OFFSET, "gl_MaxProgramTexelOffset"},
        {GL_MAX_GEOMETRY_INPUT_COMPONENTS, "gl_MaxGeometryInputComponents"},
        {GL_MAX_GEOMETRY_OUTPUT_COMPONENTS, "gl_MaxGeometryOutputComponents"},
        {GL_MAX_GEOMETRY_TEXTURE_IMAGE_UNITS, "gl_MaxGeometryTextureImageUnits"},
        {GL_MAX_GEOMETRY_OUTPUT_VERTICES, "gl_MaxGeometryOutputVertices"},
        {GL_MAX_GEOMETRY_TOTAL_OUTPUT_COMPONENTS, "gl_MaxGeometryTotalOutputComponents"},
        {GL_MAX_GEOMETRY_UNIFORM_COMPONENTS, "gl_MaxGeometryUniformComponents"},
        {GL_MAX_PATCH_VERTICES, "gl_MaxPatchVertices"},
        {GL_MAX_TESS_GEN_LEVEL, "gl_MaxTessGenLevel"},
        {GL_MAX_TESS_CONTROL_INPUT_COMPONENTS, "gl_MaxTessControlInputComponents"},
        {GL_MAX_TESS_CONTROL_OUTPUT_COMPONENTS, "gl_MaxTessControlOutputComponents"},
        {GL_MAX_TESS_CONTROL_TEXTURE_IMAGE_UNITS, "gl_MaxTessControlTextureImageUnits"},
        {GL_MAX_TESS_CONTROL_UNIFORM_COMPONENTS, "gl_MaxTessControlUniformComponents"},
        {GL_MAX_TESS_CONTROL_TOTAL_OUTPUT_COMPONENTS, "gl_MaxTessControlTotalOutputComponents"},
        {GL_MAX_TESS_EVALUATION_INPUT_COMPONENTS, "gl_MaxTessEvaluationInputComponents"},
        {GL_MAX_TESS_EVALUATION_OUTPUT_COMPONENTS, "gl_MaxTessEvaluationOutputComponents"},
        {GL_MAX_TESS_EVALUATION_TEXTURE_IMAGE_UNITS, "gl_MaxTessEvaluationTextureImageUnits"},
        {GL_MAX_TESS_EVALUATION_UNIFORM_COMPONENTS, "gl_MaxTessEvaluationUniformComponents"},
        {GL_MAX_TESS_PATCH_COMPONENTS, "gl_MaxTessPatchComponents"},
        {GL_MAX_TRANSFORM_FEEDBACK_BUFFERS, "gl_MaxTransformFeedbackBuffers"},
        {GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS, "gl_MaxTransformFeedbackInterleavedComponents"},
        // gl_MaxAtomicCounterBindings is glslang's name for the binding count; the GL spelling is
        // GL_MAX_ATOMIC_COUNTER_BUFFER_BINDINGS.
        {GL_MAX_ATOMIC_COUNTER_BUFFER_BINDINGS, "gl_MaxAtomicCounterBindings"},
        {GL_MAX_ATOMIC_COUNTER_BUFFER_SIZE, "gl_MaxAtomicCounterBufferSize"},
    };

    // The compile runs against a captured env, exactly as the pipeline's does - that is what
    // makes "the resource table" mean the same thing here as it does in production.
    const auto env = MG_Util::ShaderTranspiler::CaptureCompileEnv();
    for (const LimitPair& pair : pairs) {
        GLint reported = -424242;
        MG_Impl::GLImpl::GetIntegerv(pair.pname, &reported);
        EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR)
            << pair.builtin << "'s pname is not answerable at all";

        // A negative array size is a compile error, so the stage only compiles when the built-in
        // equals what the query just reported. Two-sided by construction: a resource table that
        // is too permissive fails it exactly like one that is too tight. One shader per pair, so
        // a failure names the limit instead of reporting "something disagreed".
        const String source = String("#version 460 core\nout vec4 mgColor;\nconst int mgAgree = (") +
                              pair.builtin + " == " + std::to_string(reported) +
                              ") ? 1 : -1;\nint mgProbe[mgAgree];\nvoid main() { mgProbe[0] = 0; mgColor = "
                              "vec4(float(mgProbe[0])); }\n";
        auto compiled = MG_Util::ShaderTranspiler::ShaderCompiler::CompileShader({
            .shaderType = GL_FRAGMENT_SHADER,
            .sourceStr = source,
            .env = env.get(),
        });
        EXPECT_TRUE(compiled) << pair.builtin << " does not equal glGetIntegerv's " << reported << ":\n"
                              << (compiled ? String() : compiled.error().log);
    }

    MG_Backend::pActiveBackendObject = Move(previousBackend);
    MG_State::pGLContext = Move(previousContext);
}

// GL_MAX_ELEMENT_INDEX is 64-bit state whose required value (2^32-1) does not fit a GLint, so it
// needs its own case in BOTH widths: the 64-bit query has to answer 4294967295 and the 32-bit one
// has to saturate, per the GL state-query conversion rules. It used to be a single `1024 * 1024;
// // TODO` in the 32-bit table, and glGetInteger64v - which is how the conformance suite reads it
// - widened that.
TEST(GetterSanity, MaxElementIndexIsTheFull32BitIndexCeiling) {
    using namespace MobileGL;

    auto previousContext = Move(MG_State::pGLContext);
    auto previousBackend = Move(MG_Backend::pActiveBackendObject);
    MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();
    MG_Backend::pActiveBackendObject = MakeUnique<DynamicParameterBackend>(MG_Backend::DynamicBackendParameters{});

    GLint64 wide = -1;
    MG_Impl::GLImpl::GetInteger64v(GL_MAX_ELEMENT_INDEX, &wide);
    EXPECT_EQ(wide, static_cast<GLint64>(0xFFFFFFFFLL));
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    GLint narrow = -1;
    MG_Impl::GLImpl::GetIntegerv(GL_MAX_ELEMENT_INDEX, &narrow);
    EXPECT_EQ(narrow, INT32_MAX) << "the 32-bit query must saturate, not truncate or wrap";
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Backend::pActiveBackendObject = Move(previousBackend);
    MG_State::pGLContext = Move(previousContext);
}

// GL 4.6 core table 23.53 gives GL_MAX_SAMPLES a minimum of four and the three per-category
// ceilings a minimum of ONE. Flooring the latter at four is the advertised-caps lie that made
// KHR-GL46.sample_variables.mask.rgba8i run at all: the frontend promised four integer samples,
// the backend clamped the realised allocation to the one the driver can back, and the application
// wrote per-sample data it could never read.
TEST(GetterSanity, PerCategoryMultisampleCeilingsAreProbedRatherThanFlooredAtFour) {
    using namespace MobileGL;

    auto previousContext = Move(MG_State::pGLContext);
    auto previousBackend = Move(MG_Backend::pActiveBackendObject);
    MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();

    MG_Backend::DynamicBackendParameters params;
    params.MaxSamples = 4;
    params.MaxColorTextureSamples = 4;
    params.MaxDepthTextureSamples = 2;
    params.MaxIntegerSamples = 1;
    MG_Backend::pActiveBackendObject = MakeUnique<DynamicParameterBackend>(params);

    GLint reported = -1;
    MG_Impl::GLImpl::GetIntegerv(GL_MAX_INTEGER_SAMPLES, &reported);
    EXPECT_EQ(reported, 1) << "an integer multisample texture is backed by one sample here, and "
                              "saying otherwise is what the application allocates against";
    MG_Impl::GLImpl::GetIntegerv(GL_MAX_DEPTH_TEXTURE_SAMPLES, &reported);
    EXPECT_EQ(reported, 2);
    MG_Impl::GLImpl::GetIntegerv(GL_MAX_COLOR_TEXTURE_SAMPLES, &reported);
    EXPECT_EQ(reported, 4);
    // ...while GL_MAX_SAMPLES keeps its floor of four, which is the one the spec really requires.
    params.MaxSamples = 1;
    MG_Backend::pActiveBackendObject = MakeUnique<DynamicParameterBackend>(params);
    MG_Impl::GLImpl::GetIntegerv(GL_MAX_SAMPLES, &reported);
    EXPECT_EQ(reported, 4);
    EXPECT_EQ(MG_Impl::GLImpl::GetError(), GL_NO_ERROR);

    MG_Backend::pActiveBackendObject = Move(previousBackend);
    MG_State::pGLContext = Move(previousContext);
}

// GL_ARB_cull_distance below #version 450, which is the band the conformance suite actually
// compiles in: cull_distance.coverage emits its compute shader at "#version 420 core" with
// `#extension GL_ARB_cull_distance : require` and reads gl_MaxCullDistances. Registering the
// extension name alone was not enough - `require` started succeeding while the constants stayed
// gated on 450, so the shader traded one error for another.
//
// The three cases below are the whole contract: the macro must be true exactly where the feature
// is, the constants must exist under the extension, and using the feature WITHOUT the extension
// must still fail (otherwise the gate is decorative).
TEST(ShaderCompilerSanity, ArbCullDistanceIsUsableBelow450) {
    using namespace MobileGL;

    auto previousContext = Move(MG_State::pGLContext);
    auto previousBackend = Move(MG_Backend::pActiveBackendObject);
    MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();
    MG_Backend::pActiveBackendObject = MakeUnique<DynamicParameterBackend>(MG_Backend::DynamicBackendParameters{});
    const auto env = MG_Util::ShaderTranspiler::CaptureCompileEnv();

    const auto compileFragment = [&env](const String& source) {
        return MG_Util::ShaderTranspiler::ShaderCompiler::CompileShader({
            .shaderType = GL_FRAGMENT_SHADER,
            .sourceStr = source,
            .env = env.get(),
        });
    };

    // The coverage shader's shape, reduced to a fragment stage: require the extension, then read
    // the constant it brings.
    const String withExtension = R"(#version 420 core
#extension GL_ARB_cull_distance : require
out vec4 mgColor;
void main() { mgColor = vec4(float(gl_MaxCullDistances + gl_MaxCombinedClipAndCullDistances)); }
)";
    auto extensionCompiled = compileFragment(withExtension);
    EXPECT_TRUE(extensionCompiled) << (extensionCompiled ? String() : extensionCompiled.error().log);

    // The macro has to agree with that, or the standard `#ifdef` probe lies in one direction or
    // the other. It is defined from 400 up, where the built-ins exist...
    const String macroProbe420 = R"(#version 420 core
out vec4 mgColor;
#ifndef GL_ARB_cull_distance
#error GL_ARB_cull_distance should be defined at 420
#endif
void main() { mgColor = vec4(0.0); }
)";
    auto macro420 = compileFragment(macroProbe420);
    EXPECT_TRUE(macro420) << (macro420 ? String() : macro420.error().log);

    // ...and NOT below it, where they do not. A shader whose `#ifdef GL_ARB_cull_distance` branch
    // reads gl_MaxCullDistances used to take that branch at 330 and fail to compile.
    const String macroProbe330 = R"(#version 330 core
out vec4 mgColor;
#ifdef GL_ARB_cull_distance
#error GL_ARB_cull_distance must not be advertised where the built-ins do not exist
#endif
void main() { mgColor = vec4(0.0); }
)";
    auto macro330 = compileFragment(macroProbe330);
    EXPECT_TRUE(macro330) << (macro330 ? String() : macro330.error().log);

    // The gate is real: below 450 the constants are reachable ONLY through the extension.
    const String withoutExtension = R"(#version 420 core
out vec4 mgColor;
void main() { mgColor = vec4(float(gl_MaxCullDistances)); }
)";
    EXPECT_FALSE(compileFragment(withoutExtension))
        << "gl_MaxCullDistances must require GL_ARB_cull_distance below #version 450";

    // ...and at 450 it is core, so no directive is needed.
    const String core450 = R"(#version 450 core
out vec4 mgColor;
void main() { mgColor = vec4(float(gl_MaxCullDistances)); }
)";
    auto coreCompiled = compileFragment(core450);
    EXPECT_TRUE(coreCompiled) << (coreCompiled ? String() : coreCompiled.error().log);

    MG_Backend::pActiveBackendObject = Move(previousBackend);
    MG_State::pGLContext = Move(previousContext);
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

TEST(DirectVulkanSanity, ReadbackUsesTheSourceFormatTexelSize) {
    using MobileGL::MG_Backend::DirectVulkan::VulkanRenderer;

    EXPECT_EQ(VulkanRenderer::GetReadbackTexelSize(VK_FORMAT_R8G8B8A8_UNORM), 4u);
    EXPECT_EQ(VulkanRenderer::GetReadbackTexelSize(VK_FORMAT_R16G16B16A16_SFLOAT), 8u);
    EXPECT_EQ(VulkanRenderer::GetReadbackTexelSize(VK_FORMAT_R32G32B32A32_SFLOAT), 16u);
}

TEST(DirectVulkanSanity, DefaultFramebufferQuarterTurnReadbackMapsRectAndPixels) {
    using MobileGL::MG_Backend::DirectVulkan::VulkanRenderer;
    using MobileGL::Uint8;

    VkOffset2D offset{};
    VkExtent2D copyExtent{};
    ASSERT_TRUE(VulkanRenderer::MapDefaultFramebufferReadbackRect(
        1, 0, 2, 1, VkExtent2D{2, 3}, VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR,
        &offset, &copyExtent));
    EXPECT_EQ(offset.x, 0);
    EXPECT_EQ(offset.y, 1);
    EXPECT_EQ(copyExtent.width, 1u);
    EXPECT_EQ(copyExtent.height, 2u);

    ASSERT_TRUE(VulkanRenderer::MapDefaultFramebufferReadbackRect(
        1, 0, 2, 1, VkExtent2D{2, 3}, VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR,
        &offset, &copyExtent));
    EXPECT_EQ(offset.x, 1);
    EXPECT_EQ(offset.y, 0);
    EXPECT_EQ(copyExtent.width, 1u);
    EXPECT_EQ(copyExtent.height, 2u);

    // Logical GL rows, bottom to top, are abc / def. The display-oriented swapchain blocks are
    // transposed in opposite directions for 90 and 270 degrees.
    const Uint8 raw90[] = {'a', 'd', 'b', 'e', 'c', 'f'};
    const Uint8 raw270[] = {'f', 'c', 'e', 'b', 'd', 'a'};
    const Uint8 expected[] = {'a', 'b', 'c', 'd', 'e', 'f'};
    Uint8 result[sizeof(expected)]{};

    ASSERT_TRUE(VulkanRenderer::RemapDefaultFramebufferReadback(
        raw90, 3, 2, VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR, 1, result));
    EXPECT_TRUE(std::equal(std::begin(expected), std::end(expected), std::begin(result)));

    std::fill(std::begin(result), std::end(result), 0);
    ASSERT_TRUE(VulkanRenderer::RemapDefaultFramebufferReadback(
        raw270, 3, 2, VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR, 1, result));
    EXPECT_TRUE(std::equal(std::begin(expected), std::end(expected), std::begin(result)));
}

TEST(DirectVulkanSanity, ReadbackConvertsRgba8AndRgba16fPixels) {
    using MobileGL::MG_Backend::DirectVulkan::VulkanRenderer;
    using MobileGL::MG_Util::EncodeFloatToHalfBits;

    const MobileGL::Uint8 rgba8[] = {17, 34, 51, 68, 85, 102, 119, 136};
    MobileGL::Uint8 rgba8Result[sizeof(rgba8)]{};
    ASSERT_TRUE(VulkanRenderer::ConvertReadbackPixels(
        rgba8, VK_FORMAT_R8G8B8A8_UNORM, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE,
        sizeof(rgba8Result), rgba8Result));
    EXPECT_TRUE(std::equal(std::begin(rgba8), std::end(rgba8), std::begin(rgba8Result)));

    const MobileGL::Uint8 bgra8[] = {51, 34, 17, 68};
    MobileGL::Uint8 bgra8Result[4]{};
    ASSERT_TRUE(VulkanRenderer::ConvertReadbackPixels(
        bgra8, VK_FORMAT_B8G8R8A8_UNORM, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
        sizeof(bgra8Result), bgra8Result));
    const MobileGL::Uint8 expectedBgra8[] = {17, 34, 51, 68};
    EXPECT_TRUE(std::equal(std::begin(expectedBgra8), std::end(expectedBgra8), std::begin(bgra8Result)));

    const MobileGL::Uint16 rgba16f[] = {
        EncodeFloatToHalfBits(-0.25f), EncodeFloatToHalfBits(0.5f), EncodeFloatToHalfBits(1.5f),
        EncodeFloatToHalfBits(1.0f), EncodeFloatToHalfBits(0.25f), EncodeFloatToHalfBits(0.0f),
        EncodeFloatToHalfBits(1.0f), EncodeFloatToHalfBits(0.5f),
        EncodeFloatToHalfBits(0.75f), EncodeFloatToHalfBits(0.125f), EncodeFloatToHalfBits(-1.0f),
        EncodeFloatToHalfBits(2.0f), EncodeFloatToHalfBits(1.0f), EncodeFloatToHalfBits(0.75f),
        EncodeFloatToHalfBits(0.25f), EncodeFloatToHalfBits(0.0f),
    };
    constexpr MobileGL::SizeT kDestinationRowStride = 12;
    MobileGL::Uint8 rgba16fResult[kDestinationRowStride * 2];
    std::fill(std::begin(rgba16fResult), std::end(rgba16fResult), 0xCD);
    ASSERT_TRUE(VulkanRenderer::ConvertReadbackPixels(
        reinterpret_cast<const MobileGL::Uint8*>(rgba16f), VK_FORMAT_R16G16B16A16_SFLOAT,
        2, 2, GL_RGBA, GL_UNSIGNED_BYTE, kDestinationRowStride, rgba16fResult));
    const MobileGL::Uint8 expectedRgba16fRow0[] = {0, 128, 255, 255, 64, 0, 255, 128};
    const MobileGL::Uint8 expectedRgba16fRow1[] = {191, 32, 0, 255, 255, 191, 64, 0};
    EXPECT_TRUE(std::equal(std::begin(expectedRgba16fRow0), std::end(expectedRgba16fRow0),
                           std::begin(rgba16fResult)));
    EXPECT_TRUE(std::equal(std::begin(expectedRgba16fRow1), std::end(expectedRgba16fRow1),
                           std::begin(rgba16fResult) + kDestinationRowStride));
    EXPECT_TRUE(std::all_of(std::begin(rgba16fResult) + 8,
                            std::begin(rgba16fResult) + kDestinationRowStride,
                            [](MobileGL::Uint8 value) { return value == 0xCD; }));

    MobileGL::Float rgba16fFloatResult[16]{};
    ASSERT_TRUE(VulkanRenderer::ConvertReadbackPixels(
        reinterpret_cast<const MobileGL::Uint8*>(rgba16f), VK_FORMAT_R16G16B16A16_SFLOAT,
        2, 2, GL_RGBA, GL_FLOAT, sizeof(MobileGL::Float) * 8,
        reinterpret_cast<MobileGL::Uint8*>(rgba16fFloatResult)));
    EXPECT_FLOAT_EQ(rgba16fFloatResult[0], -0.25f);
    EXPECT_FLOAT_EQ(rgba16fFloatResult[1], 0.5f);
    EXPECT_FLOAT_EQ(rgba16fFloatResult[2], 1.5f);
    EXPECT_FLOAT_EQ(rgba16fFloatResult[3], 1.0f);
}

TEST(DirectVulkanSanity, ReadbackDecodesSingleChannel32BitFormats) {
    using MobileGL::MG_Backend::DirectVulkan::VulkanRenderer;

    // The reinterpretation feature makes R32F/R32UI-class images common readback sources
    // (iterationRP custom images). Missing channels take GL defaults: 0 for GB, 1 for alpha.
    const MobileGL::Float r32f[] = {0.75f, -2.0f};
    MobileGL::Float r32fResult[8]{};
    ASSERT_TRUE(VulkanRenderer::ConvertReadbackPixels(
        reinterpret_cast<const MobileGL::Uint8*>(r32f), VK_FORMAT_R32_SFLOAT,
        2, 1, GL_RGBA, GL_FLOAT, sizeof(MobileGL::Float) * 8,
        reinterpret_cast<MobileGL::Uint8*>(r32fResult)));
    EXPECT_FLOAT_EQ(r32fResult[0], 0.75f);
    EXPECT_FLOAT_EQ(r32fResult[1], 0.0f);
    EXPECT_FLOAT_EQ(r32fResult[2], 0.0f);
    EXPECT_FLOAT_EQ(r32fResult[3], 1.0f);
    EXPECT_FLOAT_EQ(r32fResult[4], -2.0f);

    const MobileGL::Uint32 r32ui[] = {12345u};
    MobileGL::Float r32uiResult[4]{};
    ASSERT_TRUE(VulkanRenderer::ConvertReadbackPixels(
        reinterpret_cast<const MobileGL::Uint8*>(r32ui), VK_FORMAT_R32_UINT,
        1, 1, GL_RGBA, GL_FLOAT, sizeof(MobileGL::Float) * 4,
        reinterpret_cast<MobileGL::Uint8*>(r32uiResult)));
    EXPECT_FLOAT_EQ(r32uiResult[0], 12345.0f);
    EXPECT_FLOAT_EQ(r32uiResult[3], 1.0f);
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

TEST(DirectVulkanSanity, SpirvStorageImageFormatsMapToVulkanFormats) {
    using MobileGL::MG_Backend::DirectVulkan::ProgramFactory;

    struct FormatCase {
        SpvImageFormat spirv;
        VkFormat vulkan;
    };
    const FormatCase cases[] = {
        {SpvImageFormatUnknown, VK_FORMAT_UNDEFINED},
        {SpvImageFormatRgba32f, VK_FORMAT_R32G32B32A32_SFLOAT},
        {SpvImageFormatRgba16f, VK_FORMAT_R16G16B16A16_SFLOAT},
        {SpvImageFormatR32f, VK_FORMAT_R32_SFLOAT},
        {SpvImageFormatRgba8, VK_FORMAT_R8G8B8A8_UNORM},
        {SpvImageFormatRgba8Snorm, VK_FORMAT_R8G8B8A8_SNORM},
        {SpvImageFormatRg32f, VK_FORMAT_R32G32_SFLOAT},
        {SpvImageFormatRg16f, VK_FORMAT_R16G16_SFLOAT},
        {SpvImageFormatR11fG11fB10f, VK_FORMAT_B10G11R11_UFLOAT_PACK32},
        {SpvImageFormatR16f, VK_FORMAT_R16_SFLOAT},
        {SpvImageFormatRgba16, VK_FORMAT_R16G16B16A16_UNORM},
        // A2**B**10G10R10, matching MGToVk::ConvertTextureInternalFormatToVkFormat's RGB10A2:
        // the view format and the image format have to name the same bit layout, and
        // GL_UNSIGNED_INT_2_10_10_10_REV is A2B10G10R10. A2R10G10B10 transposes R and B.
        {SpvImageFormatRgb10A2, VK_FORMAT_A2B10G10R10_UNORM_PACK32},
        {SpvImageFormatRg16, VK_FORMAT_R16G16_UNORM},
        {SpvImageFormatRg8, VK_FORMAT_R8G8_UNORM},
        {SpvImageFormatR16, VK_FORMAT_R16_UNORM},
        {SpvImageFormatR8, VK_FORMAT_R8_UNORM},
        {SpvImageFormatRgba16Snorm, VK_FORMAT_R16G16B16A16_SNORM},
        {SpvImageFormatRg16Snorm, VK_FORMAT_R16G16_SNORM},
        {SpvImageFormatRg8Snorm, VK_FORMAT_R8G8_SNORM},
        {SpvImageFormatR16Snorm, VK_FORMAT_R16_SNORM},
        {SpvImageFormatR8Snorm, VK_FORMAT_R8_SNORM},
        {SpvImageFormatRgba32i, VK_FORMAT_R32G32B32A32_SINT},
        {SpvImageFormatRgba16i, VK_FORMAT_R16G16B16A16_SINT},
        {SpvImageFormatRgba8i, VK_FORMAT_R8G8B8A8_SINT},
        {SpvImageFormatR32i, VK_FORMAT_R32_SINT},
        {SpvImageFormatRg32i, VK_FORMAT_R32G32_SINT},
        {SpvImageFormatRg16i, VK_FORMAT_R16G16_SINT},
        {SpvImageFormatRg8i, VK_FORMAT_R8G8_SINT},
        {SpvImageFormatR16i, VK_FORMAT_R16_SINT},
        {SpvImageFormatR8i, VK_FORMAT_R8_SINT},
        {SpvImageFormatRgba32ui, VK_FORMAT_R32G32B32A32_UINT},
        {SpvImageFormatRgba16ui, VK_FORMAT_R16G16B16A16_UINT},
        {SpvImageFormatRgba8ui, VK_FORMAT_R8G8B8A8_UINT},
        {SpvImageFormatR32ui, VK_FORMAT_R32_UINT},
        {SpvImageFormatRgb10a2ui, VK_FORMAT_A2B10G10R10_UINT_PACK32},
        {SpvImageFormatRg32ui, VK_FORMAT_R32G32_UINT},
        {SpvImageFormatRg16ui, VK_FORMAT_R16G16_UINT},
        {SpvImageFormatRg8ui, VK_FORMAT_R8G8_UINT},
        {SpvImageFormatR16ui, VK_FORMAT_R16_UINT},
        {SpvImageFormatR8ui, VK_FORMAT_R8_UINT},
        {SpvImageFormatR64ui, VK_FORMAT_R64_UINT},
        {SpvImageFormatR64i, VK_FORMAT_R64_SINT},
    };

    for (const auto& testCase : cases) {
        EXPECT_EQ(ProgramFactory::ConvertSpirvImageFormatToVkFormat(testCase.spirv), testCase.vulkan)
            << "SpvImageFormat=" << static_cast<int>(testCase.spirv);
    }
}

TEST(DirectVulkanSanity, MutableStorageImageViewsUseVulkanCompatibilityClasses) {
    using MobileGL::MG_Backend::DirectVulkan::VkTextureManager;

    EXPECT_TRUE(VkTextureManager::AreStorageImageViewFormatsCompatible(
        VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_UINT));
    EXPECT_TRUE(VkTextureManager::AreStorageImageViewFormatsCompatible(
        VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT));
    EXPECT_TRUE(VkTextureManager::AreStorageImageViewFormatsCompatible(
        VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT));
    EXPECT_TRUE(VkTextureManager::AreStorageImageViewFormatsCompatible(
        VK_FORMAT_R32_SFLOAT, VK_FORMAT_R8G8B8A8_UINT));
    EXPECT_TRUE(VkTextureManager::AreStorageImageViewFormatsCompatible(
        VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_SFLOAT));
    EXPECT_FALSE(VkTextureManager::AreStorageImageViewFormatsCompatible(
        VK_FORMAT_R32_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT));
    EXPECT_FALSE(VkTextureManager::AreStorageImageViewFormatsCompatible(
        VK_FORMAT_R32_SFLOAT, VK_FORMAT_D32_SFLOAT));
}

TEST(DirectVulkanSanity, StorageImageViewFormatUsesBindingOnlyForFormatlessFloatPolicy) {
    using MobileGL::MG_Backend::DirectVulkan::UniformManager;

    EXPECT_EQ(UniformManager::ResolveStorageImageViewFormat(
                  VK_FORMAT_UNDEFINED, GL_RGBA16F, VK_FORMAT_R16G16B16A16_UNORM, true),
              VK_FORMAT_R16G16B16A16_SFLOAT);
    EXPECT_EQ(UniformManager::ResolveStorageImageViewFormat(
                  VK_FORMAT_UNDEFINED, GL_RGBA16, VK_FORMAT_R16G16B16A16_SFLOAT, true),
              VK_FORMAT_R16G16B16A16_UNORM);
    EXPECT_EQ(UniformManager::ResolveStorageImageViewFormat(
                  VK_FORMAT_R32_UINT, GL_RGBA16F, VK_FORMAT_R32_SFLOAT, false),
              VK_FORMAT_R32_UINT);
    EXPECT_EQ(UniformManager::ResolveStorageImageViewFormat(
                  VK_FORMAT_UNDEFINED, GL_RGBA16F, VK_FORMAT_R32_SFLOAT, false),
              VK_FORMAT_R32_SFLOAT);
    EXPECT_EQ(UniformManager::ResolveStorageImageViewFormat(
                  VK_FORMAT_UNDEFINED, GL_NONE, VK_FORMAT_R16G16B16A16_SFLOAT, true),
              VK_FORMAT_UNDEFINED);
}

TEST(DirectVulkanSanity, ProgramObjectMovePreservesStorageImageFormatPolicy) {
    using MobileGL::MG_Backend::DirectVulkan::ProgramFactory;

    ProgramFactory::VkProgramObject source;
    source.storageImageFormatByBinding = {VK_FORMAT_UNDEFINED, VK_FORMAT_R32_UINT};
    source.storageImageUsesBindingFormatByBinding = {true, false};

    ProgramFactory::VkProgramObject moved(std::move(source));
    ASSERT_EQ(moved.storageImageFormatByBinding.size(), 2u);
    ASSERT_EQ(moved.storageImageUsesBindingFormatByBinding.size(), 2u);
    EXPECT_EQ(moved.storageImageFormatByBinding[0], VK_FORMAT_UNDEFINED);
    EXPECT_EQ(moved.storageImageFormatByBinding[1], VK_FORMAT_R32_UINT);
    EXPECT_TRUE(moved.storageImageUsesBindingFormatByBinding[0]);
    EXPECT_FALSE(moved.storageImageUsesBindingFormatByBinding[1]);

    ProgramFactory::VkProgramObject assigned;
    assigned = std::move(moved);
    ASSERT_EQ(assigned.storageImageFormatByBinding.size(), 2u);
    ASSERT_EQ(assigned.storageImageUsesBindingFormatByBinding.size(), 2u);
    EXPECT_TRUE(assigned.storageImageUsesBindingFormatByBinding[0]);
    EXPECT_FALSE(assigned.storageImageUsesBindingFormatByBinding[1]);
}

TEST(DirectVulkanSanity, SamplerUniformTypesPreserveTheirNumericDomain) {
    using namespace MobileGL::MG_Backend::DirectVulkan;

    EXPECT_EQ(ProgramFactory::UniformTypeToSamplerNumericDomain(GL_SAMPLER_2D),
              SamplerNumericDomain::Float);
    EXPECT_EQ(ProgramFactory::UniformTypeToSamplerNumericDomain(GL_SAMPLER_CUBE_MAP_ARRAY_SHADOW),
              SamplerNumericDomain::Float);
    EXPECT_EQ(ProgramFactory::UniformTypeToSamplerNumericDomain(GL_INT_SAMPLER_2D_ARRAY),
              SamplerNumericDomain::SignedInteger);
    EXPECT_EQ(ProgramFactory::UniformTypeToSamplerNumericDomain(GL_UNSIGNED_INT_SAMPLER_2D),
              SamplerNumericDomain::UnsignedInteger);
    EXPECT_EQ(ProgramFactory::UniformTypeToSamplerNumericDomain(GL_IMAGE_2D),
              SamplerNumericDomain::Unknown);
}

// The image half of the same question, which the sampler form above deliberately answers
// Unknown. It decides the format of the placeholder descriptor an UNBOUND image unit gets, and a
// `writeonly` declaration carries no format qualifier for it to fall back on - so an Unknown here
// is a lost draw, not a cosmetic gap.
TEST(DirectVulkanSanity, ImageUniformTypesPreserveTheirNumericDomain) {
    using namespace MobileGL::MG_Backend::DirectVulkan;

    EXPECT_EQ(ProgramFactory::UniformTypeToImageNumericDomain(GL_IMAGE_2D), SamplerNumericDomain::Float);
    EXPECT_EQ(ProgramFactory::UniformTypeToImageNumericDomain(GL_IMAGE_BUFFER), SamplerNumericDomain::Float);
    EXPECT_EQ(ProgramFactory::UniformTypeToImageNumericDomain(GL_IMAGE_CUBE_MAP_ARRAY),
              SamplerNumericDomain::Float);
    EXPECT_EQ(ProgramFactory::UniformTypeToImageNumericDomain(GL_INT_IMAGE_2D_ARRAY),
              SamplerNumericDomain::SignedInteger);
    EXPECT_EQ(ProgramFactory::UniformTypeToImageNumericDomain(GL_INT_IMAGE_BUFFER),
              SamplerNumericDomain::SignedInteger);
    EXPECT_EQ(ProgramFactory::UniformTypeToImageNumericDomain(GL_UNSIGNED_INT_IMAGE_3D),
              SamplerNumericDomain::UnsignedInteger);
    EXPECT_EQ(ProgramFactory::UniformTypeToImageNumericDomain(GL_UNSIGNED_INT_IMAGE_BUFFER),
              SamplerNumericDomain::UnsignedInteger);
    // Samplers are the other function's business, and answering for them here would let a
    // sampler binding silently take an image binding's placeholder rules.
    EXPECT_EQ(ProgramFactory::UniformTypeToImageNumericDomain(GL_SAMPLER_2D), SamplerNumericDomain::Unknown);
}

TEST(DirectVulkanSanity, SampledViewFormatMatchesSamplerNumericDomainWithoutChangingComponentLayout) {
    using namespace MobileGL::MG_Backend::DirectVulkan;

    EXPECT_EQ(VkTextureManager::ResolveSampledImageViewFormat(
                  VK_FORMAT_R32_SFLOAT, SamplerNumericDomain::UnsignedInteger),
              VK_FORMAT_R32_UINT);
    EXPECT_EQ(VkTextureManager::ResolveSampledImageViewFormat(
                  VK_FORMAT_R32_SFLOAT, SamplerNumericDomain::SignedInteger),
              VK_FORMAT_R32_SINT);
    EXPECT_EQ(VkTextureManager::ResolveSampledImageViewFormat(
                  VK_FORMAT_R32_UINT, SamplerNumericDomain::Float),
              VK_FORMAT_R32_SFLOAT);
    EXPECT_EQ(VkTextureManager::ResolveSampledImageViewFormat(
                  VK_FORMAT_R16G16B16A16_SFLOAT, SamplerNumericDomain::UnsignedInteger),
              VK_FORMAT_R16G16B16A16_UINT);
    EXPECT_EQ(VkTextureManager::ResolveSampledImageViewFormat(
                  VK_FORMAT_R8G8B8A8_UNORM, SamplerNumericDomain::UnsignedInteger),
              VK_FORMAT_R8G8B8A8_UINT);
    EXPECT_EQ(VkTextureManager::ResolveSampledImageViewFormat(
                  VK_FORMAT_R32_UINT, SamplerNumericDomain::UnsignedInteger),
              VK_FORMAT_R32_UINT);
    EXPECT_EQ(VkTextureManager::ResolveSampledImageViewFormat(
                  VK_FORMAT_B10G11R11_UFLOAT_PACK32, SamplerNumericDomain::UnsignedInteger),
              VK_FORMAT_UNDEFINED);

    // Depth/stencil formats never resolve through color-class reinterpretation; they pass
    // through unchanged so the existing depth-aspect sampled view is used. Combined
    // depth-stencil formats are multi-numeric (vkuFormatIsSampledFloat is false for them),
    // so without the passthrough a plain sampler2D/sampler2DShadow on GL_DEPTH24_STENCIL8
    // would resolve to UNDEFINED and the draw would be dropped.
    EXPECT_EQ(VkTextureManager::ResolveSampledImageViewFormat(
                  VK_FORMAT_D24_UNORM_S8_UINT, SamplerNumericDomain::Float),
              VK_FORMAT_D24_UNORM_S8_UINT);
    EXPECT_EQ(VkTextureManager::ResolveSampledImageViewFormat(
                  VK_FORMAT_D32_SFLOAT_S8_UINT, SamplerNumericDomain::Float),
              VK_FORMAT_D32_SFLOAT_S8_UINT);
    EXPECT_EQ(VkTextureManager::ResolveSampledImageViewFormat(
                  VK_FORMAT_D32_SFLOAT, SamplerNumericDomain::Float),
              VK_FORMAT_D32_SFLOAT);
    EXPECT_EQ(VkTextureManager::ResolveSampledImageViewFormat(
                  VK_FORMAT_D24_UNORM_S8_UINT, SamplerNumericDomain::UnsignedInteger),
              VK_FORMAT_D24_UNORM_S8_UINT);
    EXPECT_EQ(VkTextureManager::ResolveSampledImageViewFormat(
                  VK_FORMAT_D32_SFLOAT, SamplerNumericDomain::UnsignedInteger),
              VK_FORMAT_D32_SFLOAT);

    EXPECT_TRUE(VkTextureManager::AreSampledImageViewFormatsCompatible(
        VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_UINT));
    EXPECT_FALSE(VkTextureManager::AreSampledImageViewFormatsCompatible(
        VK_FORMAT_R32_SFLOAT, VK_FORMAT_R16G16B16A16_UINT));
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


// ---- DirectGLES readback driver-state shadows ----------------------------------------------------
// Regression coverage for the readback-path state-leak overhaul: the pixel-PBO
// binding cache, the framebuffer-binding shadow, the PACK pixel-store shadow and
// the scratch-FBO attachment shadow must (a) leave the driver in the documented
// resting state, (b) skip redundant GL calls, and (c) scrub correctly on
// deletion. All drive the real Managers.cpp implementations against a recording
// mock GLES table.
namespace {
    struct StateGuardCallLog {
        MobileGL::Vector<MobileGL::String> calls;

        MobileGL::SizeT Count(const MobileGL::String& prefix) const {
            MobileGL::SizeT n = 0;
            for (const auto& c : calls) {
                if (c.compare(0, prefix.size(), prefix) == 0) ++n;
            }
            return n;
        }
    };

    StateGuardCallLog* g_stateGuardLog = nullptr;
    GLuint g_nextStateGuardFBOId = 201;

    void SG_Log(MobileGL::String entry) {
        if (g_stateGuardLog) g_stateGuardLog->calls.push_back(MobileGL::Move(entry));
    }
    void SG_BindBuffer(GLenum target, GLuint buffer) {
        SG_Log("BindBuffer:" + std::to_string(target) + ":" + std::to_string(buffer));
    }
    void SG_BindFramebuffer(GLenum target, GLuint framebuffer) {
        SG_Log("BindFramebuffer:" + std::to_string(target) + ":" + std::to_string(framebuffer));
    }
    void SG_GetIntegerv(GLenum pname, GLint* data) {
        SG_Log("GetIntegerv:" + std::to_string(pname));
        if (data) *data = 0;
    }
    void SG_PixelStorei(GLenum pname, GLint param) {
        SG_Log("PixelStorei:" + std::to_string(pname) + ":" + std::to_string(param));
    }
    void SG_GenFramebuffers(GLsizei count, GLuint* framebuffers) {
        for (GLsizei i = 0; i < count; ++i) framebuffers[i] = g_nextStateGuardFBOId++;
    }
    void SG_FramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) {
        SG_Log("FramebufferTexture2D:" + std::to_string(target) + ":" + std::to_string(attachment) + ":" +
               std::to_string(textarget) + ":" + std::to_string(texture) + ":" + std::to_string(level));
    }
    void SG_FramebufferTextureLayer(GLenum target, GLenum attachment, GLuint texture, GLint level, GLint layer) {
        SG_Log("FramebufferTextureLayer:" + std::to_string(target) + ":" + std::to_string(attachment) + ":" +
               std::to_string(texture) + ":" + std::to_string(level) + ":" + std::to_string(layer));
    }
    void SG_ReadBuffer(GLenum src) {
        SG_Log("ReadBuffer:" + std::to_string(src));
    }
    void SG_DrawBuffers(GLsizei n, const GLenum* bufs) {
        SG_Log("DrawBuffers:" + std::to_string(n) + ":" + std::to_string(n > 0 && bufs ? bufs[0] : 0));
    }
    GLenum SG_NoError() {
        return GL_NO_ERROR;
    }

    // Installs the recording table and resets every readback driver-state shadow on
    // both ends, so these tests cannot bleed into (or inherit from) other tests.
    struct ScopedStateGuardMocks {
        ScopedStateGuardMocks(): previousFunctions(MobileGL::MG_Backend::DirectGLES::g_GLESFuncs) {
            ResetShadows();
            MobileGL::MG_External::GLESFunctionsTable functions{};
            functions.glBindBuffer = SG_BindBuffer;
            functions.glBindFramebuffer = SG_BindFramebuffer;
            functions.glGetIntegerv = SG_GetIntegerv;
            functions.glPixelStorei = SG_PixelStorei;
            functions.glGenFramebuffers = SG_GenFramebuffers;
            functions.glFramebufferTexture2D = SG_FramebufferTexture2D;
            functions.glFramebufferTextureLayer = SG_FramebufferTextureLayer;
            functions.glReadBuffer = SG_ReadBuffer;
            functions.glDrawBuffers = SG_DrawBuffers;
            functions.glGetError = SG_NoError;
            MobileGL::MG_Backend::DirectGLES::SetGLESFuncsTable(functions);
            g_stateGuardLog = &log;
        }

        ~ScopedStateGuardMocks() {
            g_stateGuardLog = nullptr;
            MobileGL::MG_Backend::DirectGLES::SetGLESFuncsTable(previousFunctions);
            ResetShadows();
        }

        ScopedStateGuardMocks(const ScopedStateGuardMocks&) = delete;
        ScopedStateGuardMocks& operator=(const ScopedStateGuardMocks&) = delete;

        static void ResetShadows() {
            MobileGL::MG_Backend::DirectGLES::BufferImpl::InvalidatePixelBufferBindingCaches();
            MobileGL::MG_Backend::DirectGLES::FramebufferImpl::InvalidateFramebufferBindingCache();
            MobileGL::MG_Backend::DirectGLES::PixelStoreImpl::InvalidatePackStateCache();
            MobileGL::MG_Backend::DirectGLES::ScratchFBOImpl::OnBackendContextDestroyed();
        }

        StateGuardCallLog log;
        MobileGL::MG_External::GLESFunctionsTable previousFunctions;
    };
} // namespace

TEST(DirectGLESStateGuards, PixelPackBindingCacheSkipsRedundantBindsAndRestsAtZero) {
    using namespace MobileGL::MG_Backend::DirectGLES;
    ScopedStateGuardMocks mocks;

    BufferImpl::BindPixelPackBufferId(5);
    EXPECT_EQ(mocks.log.Count("BindBuffer:"), 1u);
    BufferImpl::BindPixelPackBufferId(5); // redundant: must not reach the driver
    EXPECT_EQ(mocks.log.Count("BindBuffer:"), 1u);
    BufferImpl::BindPixelPackBufferId(0); // scope exit: resting state
    EXPECT_EQ(mocks.log.Count("BindBuffer:"), 2u);
    BufferImpl::BindPixelPackBufferId(0);
    EXPECT_EQ(mocks.log.Count("BindBuffer:"), 2u);

    // After invalidation (MakeCurrent / context reset) the first bind must reach
    // the driver again even for the same value.
    BufferImpl::InvalidatePixelBufferBindingCaches();
    BufferImpl::BindPixelPackBufferId(0);
    EXPECT_EQ(mocks.log.Count("BindBuffer:"), 3u);
}

TEST(DirectGLESStateGuards, FramebufferBindingShadowPinsOnceThenSkips) {
    using namespace MobileGL::MG_Backend::DirectGLES;
    ScopedStateGuardMocks mocks;

    // Cold path: one driver query pins the shadow; further reads are free.
    (void)FramebufferImpl::CurrentFramebufferBinding(MobileGL::FramebufferTarget::Read);
    EXPECT_EQ(mocks.log.Count("GetIntegerv:"), 1u);
    (void)FramebufferImpl::CurrentFramebufferBinding(MobileGL::FramebufferTarget::Read);
    EXPECT_EQ(mocks.log.Count("GetIntegerv:"), 1u);

    FramebufferImpl::BindFramebufferId(GL_READ_FRAMEBUFFER, 7);
    EXPECT_EQ(mocks.log.Count("BindFramebuffer:"), 1u);
    FramebufferImpl::BindFramebufferId(GL_READ_FRAMEBUFFER, 7);
    EXPECT_EQ(mocks.log.Count("BindFramebuffer:"), 1u);
    // GL_FRAMEBUFFER touches both targets; DRAW is still unknown so it must bind.
    FramebufferImpl::BindFramebufferId(GL_FRAMEBUFFER, 7);
    EXPECT_EQ(mocks.log.Count("BindFramebuffer:"), 2u);
    // Both halves now match: no further calls for either single target.
    FramebufferImpl::BindFramebufferId(GL_DRAW_FRAMEBUFFER, 7);
    FramebufferImpl::BindFramebufferId(GL_READ_FRAMEBUFFER, 7);
    FramebufferImpl::BindFramebufferId(GL_FRAMEBUFFER, 7);
    EXPECT_EQ(mocks.log.Count("BindFramebuffer:"), 2u);
    EXPECT_EQ(FramebufferImpl::CurrentFramebufferBinding(MobileGL::FramebufferTarget::Draw), 7u);
    EXPECT_EQ(mocks.log.Count("GetIntegerv:"), 1u); // shadow answered, no new query
}

TEST(DirectGLESStateGuards, PackStateShadowAppliesMinimalDeltas) {
    using namespace MobileGL::MG_Backend::DirectGLES;
    ScopedStateGuardMocks mocks;

    // First application pins all four parameters.
    PixelStoreImpl::ApplyPackState(PixelStoreImpl::PackState{4, 0, 0, 0});
    EXPECT_EQ(mocks.log.Count("PixelStorei:"), 4u);
    // Identical state: zero driver calls.
    PixelStoreImpl::ApplyPackState(PixelStoreImpl::PackState{4, 0, 0, 0});
    EXPECT_EQ(mocks.log.Count("PixelStorei:"), 4u);
    // One field changed: exactly one driver call.
    PixelStoreImpl::ApplyPackState(PixelStoreImpl::PackState{1, 0, 0, 0});
    EXPECT_EQ(mocks.log.Count("PixelStorei:"), 5u);

    const auto current = PixelStoreImpl::CurrentPackState();
    EXPECT_EQ(current.Alignment, 1);
    EXPECT_EQ(current.RowLength, 0);
    EXPECT_EQ(current.SkipRows, 0);
    EXPECT_EQ(current.SkipPixels, 0);
}

TEST(DirectGLESStateGuards, ScratchFBODetachesCrossAspectResidue) {
    using namespace MobileGL::MG_Backend::DirectGLES;
    ScopedStateGuardMocks mocks;

    auto& fb = ScratchFBOImpl::TempFramebuffer();
    EXPECT_NE(ScratchFBOImpl::EnsureId(fb), 0u);

    // A depth copy leaves a DEPTH_STENCIL attachment (the pre-fix code never
    // detached it, wedging every later color readback through this FBO).
    ScratchFBOImpl::EnsureDepthAttachment2D(fb, GL_DRAW_FRAMEBUFFER, 11, GL_TEXTURE_2D, 0, /*withStencil=*/true);
    const MobileGL::String dsAttach = "FramebufferTexture2D:" + std::to_string(GL_DRAW_FRAMEBUFFER) + ":" +
                                      std::to_string(GL_DEPTH_STENCIL_ATTACHMENT);
    EXPECT_EQ(mocks.log.Count(dsAttach), 1u);

    // The next color use must detach the stale depth-stencil attachment exactly once.
    mocks.log.calls.clear();
    ScratchFBOImpl::EnsureColorAttachment2D(fb, GL_READ_FRAMEBUFFER, 22, GL_TEXTURE_2D, 0);
    const MobileGL::String dsDetach = "FramebufferTexture2D:" + std::to_string(GL_READ_FRAMEBUFFER) + ":" +
                                      std::to_string(GL_DEPTH_STENCIL_ATTACHMENT) + ":" +
                                      std::to_string(GL_TEXTURE_2D) + ":0:0";
    const MobileGL::String colorAttach = "FramebufferTexture2D:" + std::to_string(GL_READ_FRAMEBUFFER) + ":" +
                                         std::to_string(GL_COLOR_ATTACHMENT0) + ":" +
                                         std::to_string(GL_TEXTURE_2D) + ":22:0";
    EXPECT_EQ(mocks.log.Count(dsDetach), 1u);
    EXPECT_EQ(mocks.log.Count(colorAttach), 1u);

    // Back-to-back identical color use: no driver traffic at all.
    mocks.log.calls.clear();
    ScratchFBOImpl::EnsureColorAttachment2D(fb, GL_READ_FRAMEBUFFER, 22, GL_TEXTURE_2D, 0);
    EXPECT_EQ(mocks.log.Count("FramebufferTexture2D:"), 0u);
}

TEST(DirectGLESStateGuards, ScratchFBOTextureDeletionForcesFullScrub) {
    using namespace MobileGL::MG_Backend::DirectGLES;
    ScopedStateGuardMocks mocks;

    auto& fb = ScratchFBOImpl::TempFramebuffer();
    ScratchFBOImpl::EnsureId(fb);
    ScratchFBOImpl::EnsureColorAttachment2D(fb, GL_READ_FRAMEBUFFER, 22, GL_TEXTURE_2D, 0);

    // The attached texture id dies: the shadow can no longer vouch for the FBO
    // (ES does not auto-detach from unbound FBOs, and the name may be recycled),
    // so the next use must scrub and re-attach instead of skipping.
    ScratchFBOImpl::NoteTextureIdDeleted(22);
    mocks.log.calls.clear();
    ScratchFBOImpl::EnsureColorAttachment2D(fb, GL_READ_FRAMEBUFFER, 22, GL_TEXTURE_2D, 0);
    EXPECT_GE(mocks.log.Count("FramebufferTexture2D:"), 2u); // scrub (color + depth) ...
    const MobileGL::String colorAttach = "FramebufferTexture2D:" + std::to_string(GL_READ_FRAMEBUFFER) + ":" +
                                         std::to_string(GL_COLOR_ATTACHMENT0) + ":" +
                                         std::to_string(GL_TEXTURE_2D) + ":22:0";
    EXPECT_EQ(mocks.log.Count(colorAttach), 1u); // ... then the real re-attach
}

TEST(DirectGLESStateGuards, ScratchFBOReadDrawBufferStateCached) {
    using namespace MobileGL::MG_Backend::DirectGLES;
    ScopedStateGuardMocks mocks;

    auto& fb = ScratchFBOImpl::BlitReadFramebuffer();
    ScratchFBOImpl::EnsureId(fb);

    // Fresh FBOs default to COLOR_ATTACHMENT0 for both buffers: no call needed.
    ScratchFBOImpl::EnsureReadBuffer(fb, GL_COLOR_ATTACHMENT0);
    EXPECT_EQ(mocks.log.Count("ReadBuffer:"), 0u);
    // Depth blits want GL_NONE; the transition costs one call, repeats are free.
    ScratchFBOImpl::EnsureReadBuffer(fb, GL_NONE);
    ScratchFBOImpl::EnsureReadBuffer(fb, GL_NONE);
    EXPECT_EQ(mocks.log.Count("ReadBuffer:"), 1u);
    ScratchFBOImpl::EnsureDrawBuffer(fb, GL_NONE);
    ScratchFBOImpl::EnsureDrawBuffer(fb, GL_NONE);
    EXPECT_EQ(mocks.log.Count("DrawBuffers:"), 1u);
}

namespace {
    MobileGL::Vector<GLuint>* g_deletedTextureIds = nullptr;

    void SG_DeleteTextures(GLsizei count, const GLuint* textures) {
        if (!g_deletedTextureIds) return;
        for (GLsizei i = 0; i < count; ++i) g_deletedTextureIds->push_back(textures[i]);
    }

    // Clears the recording hook even when a gtest assertion unwinds the test body
    // (a dangling pointer to the dead stack vector would corrupt later tests).
    struct ScopedDeletedTextureRecording {
        explicit ScopedDeletedTextureRecording(MobileGL::Vector<GLuint>& sink) { g_deletedTextureIds = &sink; }
        ~ScopedDeletedTextureRecording() { g_deletedTextureIds = nullptr; }
        ScopedDeletedTextureRecording(const ScopedDeletedTextureRecording&) = delete;
        ScopedDeletedTextureRecording& operator=(const ScopedDeletedTextureRecording&) = delete;
    };
} // namespace

TEST(DirectGLESBackendTexture, DestructorDeletesIdAndScrubsBindingCache) {
    using namespace MobileGL::MG_Backend::DirectGLES;
    ScopedDirectGLESTextureBindings scoped; // installs glGenTextures/glBindTexture mocks + resets caches
    MobileGL::Vector<GLuint> deleted;
    ScopedDeletedTextureRecording recording(deleted);
    auto functions = g_GLESFuncs;
    functions.glDeleteTextures = SG_DeleteTextures;
    SetGLESFuncsTable(functions);

    const auto texture2DSlot = static_cast<MobileGL::SizeT>(MobileGL::TextureTarget::Texture2D);
    GLuint id = 0;
    {
        auto backendTexture = MobileGL::MakeShared<TextureImpl::BackendTextureObject>();
        id = backendTexture->GetBackendTextureId();
        ASSERT_NE(id, 0u);
        backendTexture->Bind(GL_TEXTURE_2D, 0);
        ASSERT_EQ(TextureImpl::g_boundTexturesCache[0][texture2DSlot], backendTexture.get());
    }
    // Frontend glDeleteTextures used to leak the backend id forever and leave the
    // cache pointer dangling (heap-address reuse then false-skips a later Bind).
    ASSERT_EQ(deleted.size(), 1u);
    EXPECT_EQ(deleted[0], id);
    EXPECT_EQ(TextureImpl::g_boundTexturesCache[0][texture2DSlot], nullptr);

    // A wrapper whose context died must NOT delete a foreign (recycled) name.
    {
        auto backendTexture = MobileGL::MakeShared<TextureImpl::BackendTextureObject>();
        ++g_backendContextGeneration;
        backendTexture.reset();
        --g_backendContextGeneration; // restore for later tests
        EXPECT_EQ(deleted.size(), 1u);
    }
}

// ---- DirectGLES backend twins release their driver ids --------------------------------------
// Framebuffers, renderbuffers and samplers had no destructor at all: every frontend object the
// application deleted leaked its ES twin for the whole process lifetime. An application that
// creates a framebuffer per readback (GL CTS packed_pixels.varied_rectangle makes ~3300 of them
// per case) walked the driver into a gigabyte of dead framebuffers, and past that point every
// readback through a freshly attached framebuffer came back with stale pixels.
namespace {
    struct TwinDeletionSinks {
        MobileGL::Vector<GLuint> framebuffers;
        MobileGL::Vector<GLuint> renderbuffers;
        MobileGL::Vector<GLuint> samplers;
    };

    TwinDeletionSinks* g_twinDeletionSinks = nullptr;
    GLuint g_nextTwinDriverId = 900;

    void TW_GenFramebuffers(GLsizei count, GLuint* ids) {
        for (GLsizei i = 0; i < count; ++i) ids[i] = g_nextTwinDriverId++;
    }
    void TW_DeleteFramebuffers(GLsizei count, const GLuint* ids) {
        if (!g_twinDeletionSinks) return;
        for (GLsizei i = 0; i < count; ++i) g_twinDeletionSinks->framebuffers.push_back(ids[i]);
    }
    void TW_GenRenderbuffers(GLsizei count, GLuint* ids) {
        for (GLsizei i = 0; i < count; ++i) ids[i] = g_nextTwinDriverId++;
    }
    void TW_DeleteRenderbuffers(GLsizei count, const GLuint* ids) {
        if (!g_twinDeletionSinks) return;
        for (GLsizei i = 0; i < count; ++i) g_twinDeletionSinks->renderbuffers.push_back(ids[i]);
    }
    void TW_GenSamplers(GLsizei count, GLuint* ids) {
        for (GLsizei i = 0; i < count; ++i) ids[i] = g_nextTwinDriverId++;
    }
    void TW_DeleteSamplers(GLsizei count, const GLuint* ids) {
        if (!g_twinDeletionSinks) return;
        for (GLsizei i = 0; i < count; ++i) g_twinDeletionSinks->samplers.push_back(ids[i]);
    }
    void TW_BindFramebuffer(GLenum target, GLuint framebuffer) {
        SG_Log("BindFramebuffer:" + std::to_string(target) + ":" + std::to_string(framebuffer));
    }
    void TW_BindSampler(GLuint, GLuint) {}
    void TW_BindRenderbuffer(GLenum, GLuint) {}

    // Installs a table that can create and destroy all three twin kinds, and unwinds it (plus the
    // recording pointer) even when an assertion aborts the test body.
    struct ScopedBackendTwinMocks {
        ScopedBackendTwinMocks(): previousFunctions(MobileGL::MG_Backend::DirectGLES::g_GLESFuncs) {
            MobileGL::MG_Backend::DirectGLES::FramebufferImpl::InvalidateFramebufferBindingCache();
            MobileGL::MG_External::GLESFunctionsTable functions{};
            functions.glGenFramebuffers = TW_GenFramebuffers;
            functions.glDeleteFramebuffers = TW_DeleteFramebuffers;
            functions.glBindFramebuffer = TW_BindFramebuffer;
            functions.glGenRenderbuffers = TW_GenRenderbuffers;
            functions.glDeleteRenderbuffers = TW_DeleteRenderbuffers;
            functions.glBindRenderbuffer = TW_BindRenderbuffer;
            functions.glGenSamplers = TW_GenSamplers;
            functions.glDeleteSamplers = TW_DeleteSamplers;
            functions.glBindSampler = TW_BindSampler;
            functions.glGetError = SG_NoError;
            MobileGL::MG_Backend::DirectGLES::SetGLESFuncsTable(functions);
            g_twinDeletionSinks = &sinks;
            g_stateGuardLog = &log;
        }

        ~ScopedBackendTwinMocks() {
            g_stateGuardLog = nullptr;
            g_twinDeletionSinks = nullptr;
            MobileGL::MG_Backend::DirectGLES::SetGLESFuncsTable(previousFunctions);
            MobileGL::MG_Backend::DirectGLES::FramebufferImpl::InvalidateFramebufferBindingCache();
        }

        ScopedBackendTwinMocks(const ScopedBackendTwinMocks&) = delete;
        ScopedBackendTwinMocks& operator=(const ScopedBackendTwinMocks&) = delete;

        TwinDeletionSinks sinks;
        StateGuardCallLog log;
        MobileGL::MG_External::GLESFunctionsTable previousFunctions;
    };
} // namespace

TEST(DirectGLESBackendFramebuffer, DestructorDeletesIdAndScrubsBindingShadow) {
    using namespace MobileGL::MG_Backend::DirectGLES;
    ScopedBackendTwinMocks mocks;

    GLuint id = 0;
    {
        auto backendFBO = MobileGL::MakeShared<FramebufferImpl::BackendFramebufferObject>();
        id = backendFBO->GetBackendFramebufferId();
        ASSERT_NE(id, 0u);
        backendFBO->Bind(MobileGL::FramebufferTarget::Draw);
        ASSERT_EQ(FramebufferImpl::CurrentFramebufferBinding(MobileGL::FramebufferTarget::Draw), id);
    }
    ASSERT_EQ(mocks.sinks.framebuffers.size(), 1u);
    EXPECT_EQ(mocks.sinks.framebuffers[0], id);
    // ES reverts every target bound to a deleted framebuffer to 0. The shadow has to follow, or
    // the next BindFramebufferId(0) is deduped away and the driver keeps the dead name bound.
    EXPECT_EQ(FramebufferImpl::CurrentFramebufferBinding(MobileGL::FramebufferTarget::Draw), 0u);

    // A twin whose context died must NOT delete a name a successor context may have recycled.
    {
        auto backendFBO = MobileGL::MakeShared<FramebufferImpl::BackendFramebufferObject>();
        ++g_backendContextGeneration;
        backendFBO.reset();
        --g_backendContextGeneration; // restore for later tests
        EXPECT_EQ(mocks.sinks.framebuffers.size(), 1u);
    }
}

TEST(DirectGLESBackendRenderbuffer, DestructorDeletesId) {
    using namespace MobileGL::MG_Backend::DirectGLES;
    ScopedBackendTwinMocks mocks;

    GLuint id = 0;
    {
        auto backendRBO = MobileGL::MakeShared<RenderbufferImpl::BackendRenderbufferObject>();
        id = backendRBO->GetBackendRenderbufferId();
        ASSERT_NE(id, 0u);
    }
    ASSERT_EQ(mocks.sinks.renderbuffers.size(), 1u);
    EXPECT_EQ(mocks.sinks.renderbuffers[0], id);

    {
        auto backendRBO = MobileGL::MakeShared<RenderbufferImpl::BackendRenderbufferObject>();
        ++g_backendContextGeneration;
        backendRBO.reset();
        --g_backendContextGeneration;
        EXPECT_EQ(mocks.sinks.renderbuffers.size(), 1u);
    }
}

TEST(DirectGLESBackendSampler, DestructorDeletesIdAndScrubsUnitCache) {
    using namespace MobileGL::MG_Backend::DirectGLES;
    ScopedBackendTwinMocks mocks;

    GLuint id = 0;
    {
        auto backendSampler = MobileGL::MakeShared<SamplerImpl::BackendSamplerObject>();
        id = backendSampler->GetBackendSamplerId();
        ASSERT_NE(id, 0u);
        backendSampler->Bind(3);
        ASSERT_EQ(SamplerImpl::g_boundSamplersCache[3], backendSampler.get());
    }
    ASSERT_EQ(mocks.sinks.samplers.size(), 1u);
    EXPECT_EQ(mocks.sinks.samplers[0], id);
    // glDeleteSamplers unbinds from every unit, and the next twin can land on this heap
    // address - a stale row would false-skip its Bind.
    EXPECT_EQ(SamplerImpl::g_boundSamplersCache[3], nullptr);

    {
        auto backendSampler = MobileGL::MakeShared<SamplerImpl::BackendSamplerObject>();
        ++g_backendContextGeneration;
        backendSampler.reset();
        --g_backendContextGeneration;
        EXPECT_EQ(mocks.sinks.samplers.size(), 1u);
    }
}

TEST(DirectGLESStateGuards, DefaultFramebufferBindGoesThroughShadow) {
    using namespace MobileGL::MG_Backend::DirectGLES;
    ScopedStateGuardMocks mocks;

    // The regression this guards against: binding framebuffer 0 raw while the
    // shadow keeps a user-FBO id makes the next re-bind of that FBO false-skip.
    FramebufferImpl::BindFramebufferId(GL_DRAW_FRAMEBUFFER, 7);
    FramebufferImpl::BindFramebufferId(GL_DRAW_FRAMEBUFFER, 0); // default-FBO path must use this API
    FramebufferImpl::BindFramebufferId(GL_DRAW_FRAMEBUFFER, 7); // must reach the driver again
    EXPECT_EQ(mocks.log.Count("BindFramebuffer:"), 3u);
}

// UnorderedMap::erase(iterator) contract coverage. Erase-while-iterating sweeps
// (pipeline/program cache eviction) depend on `it = map.erase(it)` naming the next
// live element exactly once: a sweep that skips entries leaks them, and one that
// runs off the end feeds garbage handles to vkDestroyPipeline (device crash on the
// first mass eviction during world load - the failure FastSTL's double-advancing
// erase actually produced before it was fixed).
//
// These pin the behaviour the call sites rely on, not one map's implementation, so
// they are written against MobileGL::UnorderedMap and survive changing what it
// names. Under ska::flat_hash_map the mechanism is different - erase backward-shifts
// the rest of the probe cluster into the hole and hands back the same slot, which
// now holds the shifted-in successor - but the observable contract is the same.
TEST(UnorderedMapSanity, EraseWhileIteratingVisitsEveryElementExactlyOnce) {
    MobileGL::UnorderedMap<MobileGL::Uint64, MobileGL::Uint64> map;
    constexpr MobileGL::Uint64 kCount = 1000;
    for (MobileGL::Uint64 key = 0; key < kCount; ++key) {
        map.emplace(key * 0x9e3779b97f4a7c15ull, key);
    }
    ASSERT_EQ(map.size(), kCount);

    // Record WHICH keys the sweep hands back, not just how many. A count alone cannot
    // tell a correct sweep from one that visits some element twice and misses another,
    // which is exactly the shape a backward-shift bug takes: the shift rewrites the
    // probe cluster, so a defect duplicates or strands elements rather than changing
    // the tally.
    std::set<MobileGL::Uint64> visitedKeys;
    MobileGL::SizeT visited = 0;
    for (auto it = map.begin(); it != map.end();) {
        const MobileGL::Uint64 key = it->first;
        EXPECT_TRUE(visitedKeys.insert(key).second) << "key " << key << " was visited twice";
        it = map.erase(it);
        ++visited;
        ASSERT_LE(visited, kCount); // runaway past end / skipped entries
    }
    EXPECT_EQ(visited, kCount);
    EXPECT_EQ(visitedKeys.size(), kCount);
    for (MobileGL::Uint64 key = 0; key < kCount; ++key) {
        EXPECT_TRUE(visitedKeys.count(key * 0x9e3779b97f4a7c15ull) != 0)
            << "key " << key << " was never visited by the sweep";
    }
    EXPECT_EQ(map.size(), 0u);
}

TEST(UnorderedMapSanity, EraseReturnsTheSuccessorElement) {
    MobileGL::UnorderedMap<MobileGL::Uint32, MobileGL::Uint32> map;
    for (MobileGL::Uint32 key = 1; key <= 64; ++key) {
        map.emplace(key, key);
    }

    // Erasing every other visited element must still visit all 64 exactly once:
    // the iterator returned by erase names the very next element, not one past it.
    MobileGL::SizeT visited = 0;
    std::set<MobileGL::Uint32> erasedKeys;
    std::set<MobileGL::Uint32> keptKeys;
    for (auto it = map.begin(); it != map.end();) {
        ++visited;
        const MobileGL::Uint32 key = it->first;
        if ((visited & 1) != 0) {
            erasedKeys.insert(key);
            it = map.erase(it);
        } else {
            keptKeys.insert(key);
            ++it;
        }
        ASSERT_LE(visited, 64u);
    }
    EXPECT_EQ(visited, 64u);
    EXPECT_EQ(erasedKeys.size() + keptKeys.size(), 64u);
    EXPECT_EQ(map.size(), keptKeys.size());

    // The interleaved erases rewrite probe clusters underneath the cursor, so the real
    // question is not how many elements the loop counted but whether the table still
    // resolves every key correctly afterwards. A stranded element stays in size() but
    // stops being findable; a duplicated one answers for a key it does not own.
    for (const MobileGL::Uint32 key : keptKeys) {
        const auto found = map.find(key);
        ASSERT_NE(found, map.end()) << "surviving key " << key << " is no longer findable";
        EXPECT_EQ(found->second, key) << "key " << key << " resolves to the wrong value";
    }
    for (const MobileGL::Uint32 key : erasedKeys) {
        EXPECT_EQ(map.find(key), map.end()) << "erased key " << key << " is still findable";
    }
}

TEST(UnorderedMapSanity, ErasingTheOnlyElementReturnsEnd) {
    using Map = MobileGL::UnorderedMap<MobileGL::Uint32, MobileGL::Uint32>;
    Map map;
    map.emplace(42u, 1u);

    // Spell the type: erase(iterator) hands back a proxy that is convertible to an
    // iterator but is not one, because finding the next element is not free and the
    // callers that discard the result should not pay for it. `auto next = ...` binds
    // the proxy instead, and then nothing it is compared against compiles.
    Map::iterator next = map.erase(map.begin());
    EXPECT_EQ(next, map.end());
    EXPECT_TRUE(map.empty());
}

namespace {
    // Records what the per-unit texture sync actually pushed at the driver: which backend
    // texture id was current when each glTexImage2D landed, and the shape it was given.
    struct TexSpecCall {
        GLuint texture;
        GLsizei width;
        GLsizei height;
    };
    MobileGL::Vector<TexSpecCall>* g_texSpecCalls = nullptr;
    GLuint g_texSpecBoundTexture = 0;

    void TS_BindTexture(GLenum, GLuint texture) { g_texSpecBoundTexture = texture; }
    void TS_ActiveTexture(GLenum) {}
    void TS_TexParameteri(GLenum, GLenum, GLint) {}
    void TS_TexParameterf(GLenum, GLenum, GLfloat) {}
    void TS_TexParameterfv(GLenum, GLenum, const GLfloat*) {}
    void TS_PixelStorei(GLenum, GLint) {}
    void TS_BindBuffer(GLenum, GLuint) {}
    void TS_TexImage2D(GLenum, GLint level, GLint, GLsizei width, GLsizei height, GLint, GLenum, GLenum,
                       const void*) {
        if (g_texSpecCalls && level == 0) {
            g_texSpecCalls->push_back({g_texSpecBoundTexture, width, height});
        }
    }

    // Clears the recording hook even when a gtest assertion unwinds the test body.
    struct ScopedTexSpecRecording {
        explicit ScopedTexSpecRecording(MobileGL::Vector<TexSpecCall>& sink) {
            g_texSpecCalls = &sink;
            g_texSpecBoundTexture = 0;
        }
        ~ScopedTexSpecRecording() { g_texSpecCalls = nullptr; }
        ScopedTexSpecRecording(const ScopedTexSpecRecording&) = delete;
        ScopedTexSpecRecording& operator=(const ScopedTexSpecRecording&) = delete;
    };

    // Gives `name` a complete single-level 2D image of the requested size without going through
    // the frontend upload path (the mock table below wires only the state-pushing entry points).
    MobileGL::SharedPtr<MobileGL::MG_State::GLState::ITextureObject> MakeComplete2DTexture(GLuint name,
                                                                                           MobileGL::Int size) {
        using namespace MobileGL;
        MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, name);
        auto object = MG_State::pGLContext->GetTextureUnitObject(0)
                          .GetBindingSlot(TextureTarget::Texture2D)
                          .GetBoundObject();
        object->SetInternalFormat(TextureInternalFormat::RGBA8);
        MG_State::GLState::AsMipmapTexture(object.get())
            ->AllocateStorage(TextureUploadTarget::Texture2D, 0, {{size, size, 1}, 4});
        return object;
    }
} // namespace

// The per-unit texture sync memo BORROWS the binding slot: an entry holds a pointer to the
// slot's shared_ptr plus the backend twin of whatever was in it when the entry was built. Its
// keys (context id, bind-generation epoch, high-water mark, sampling generation) are the primary
// guard, but they are all derived state - so the memo also has to survive a slot swap that never
// reached them.
//
// It did not. The DSA by-name emulation swapped a slot silently, every key still matched, and
// the replay drove texture A's backend twin from texture B's frontend object: A's backend
// storage was re-specified with B's shape, destroying anything A only ever had on the GPU. On
// Espryt + Iris/BSL that blanked Minecraft's 16x16 lightmap the moment a 2048x2048 shadow map
// was uploaded through a by-name call, and since the text shader multiplies by the lightmap,
// `if (color.a < 0.1) discard` then threw away every glyph in the process - HUD, menus and the
// vanilla title screen alike.
TEST(DirectGLESTextureSync, UnitMemoRefusesToDriveATwinFromAnotherTexture) {
    using namespace MobileGL;
    ScopedDirectGLESTextureBindings scoped; // fresh GLContext + registry + binding caches
    Vector<TexSpecCall> specs;
    ScopedTexSpecRecording recording(specs);

    auto functions = MG_Backend::DirectGLES::g_GLESFuncs;
    functions.glBindTexture = TS_BindTexture;
    functions.glActiveTexture = TS_ActiveTexture;
    functions.glTexImage2D = TS_TexImage2D;
    functions.glTexParameteri = TS_TexParameteri;
    functions.glTexParameterf = TS_TexParameterf;
    functions.glTexParameterfv = TS_TexParameterfv;
    functions.glPixelStorei = TS_PixelStorei;
    functions.glBindBuffer = TS_BindBuffer;
    MG_Backend::DirectGLES::SetGLESFuncsTable(functions);

    GLuint names[2] = {};
    MG_Impl::GLImpl::GenTextures(2, names);
    // `foreign` stands in for the shadow map, `resident` for the lightmap. Both are fully
    // specified BEFORE the first sync so that nothing between the two syncs can move the
    // sampling-resolution generation and invalidate the memo for an unrelated reason.
    const auto foreign = MakeComplete2DTexture(names[1], 32);
    const auto resident = MakeComplete2DTexture(names[0], 16);
    ASSERT_NE(foreign, nullptr);
    ASSERT_NE(resident, nullptr);

    // First sync: builds the memo with unit 0 -> `resident`, and gives `resident`'s twin its
    // 16x16 backend storage. The unit walk is the per-draw one, so both syncs below stand in a
    // draw - the fill is what a real glDraw* would have done before reaching this helper.
    MG_Test::ScopedPipeVerb draw(MG_Pipe::MGPipeVerb::DrawArrays);
    MG_Backend::DirectGLES::TextureImpl::SyncNeccessaryTextures();
    auto* residentSlot = MG_Backend::DirectGLES::TextureImpl::g_backendTextureObjects.Find(resident.get());
    ASSERT_NE(residentSlot, nullptr);
    ASSERT_NE(*residentSlot, nullptr);
    const GLuint residentBackendId = (*residentSlot)->GetBackendTextureId();
    ASSERT_NE(residentBackendId, 0u);
    ASSERT_FALSE(specs.empty());
    EXPECT_EQ(specs.back().texture, residentBackendId);
    EXPECT_EQ(specs.back().width, 16);

    // The hazard, reproduced at the state level: put `foreign` on the slot the memo borrows
    // WITHOUT telling the binding accounting, exactly as the by-name emulation used to.
    MG_State::pGLContext->GetTextureUnitObject(0).GetBindingSlot(TextureTarget::Texture2D).Bind(foreign);

    const SizeT specsBeforeReplay = specs.size();
    // The second draw. Its fill re-reads the memo's keys off the live context, which is the
    // premise being tested: the silent slot swap moved none of them.
    draw.Renew();
    MG_Backend::DirectGLES::TextureImpl::SyncNeccessaryTextures();

    // `foreign` must have been synced through its OWN twin...
    auto* foreignSlot = MG_Backend::DirectGLES::TextureImpl::g_backendTextureObjects.Find(foreign.get());
    ASSERT_NE(foreignSlot, nullptr);
    ASSERT_NE(*foreignSlot, nullptr);
    const GLuint foreignBackendId = (*foreignSlot)->GetBackendTextureId();
    EXPECT_NE(foreignBackendId, residentBackendId);

    // ...and above all, nothing may have re-specified the RESIDENT texture's backend storage.
    // That single call is what destroyed the lightmap.
    for (SizeT i = specsBeforeReplay; i < specs.size(); ++i) {
        EXPECT_NE(specs[i].texture, residentBackendId)
            << "the stale memo entry re-specified the resident texture's backend storage with "
            << specs[i].width << "x" << specs[i].height;
    }

    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, 0);
}

namespace {
    // What glTexParameteri actually reached the driver, and which backend texture was bound
    // when it did. The G9 probe below is a WHITE-BOX assertion (ID-19): the parameter push is
    // not observable through public GL without creating the very sampler view whose absence is
    // the subject, so the observation is taken at the driver boundary instead.
    struct TexParamCall {
        GLuint texture;
        GLenum pname;
        GLint value;
    };
    MobileGL::Vector<TexParamCall>* g_texParamCalls = nullptr;
    GLuint g_texParamBoundTexture = 0;

    void TP_BindTexture(GLenum, GLuint texture) { g_texParamBoundTexture = texture; }
    void TP_ActiveTexture(GLenum) {}
    void TP_TexParameteri(GLenum, GLenum pname, GLint value) {
        if (g_texParamCalls) {
            g_texParamCalls->push_back({g_texParamBoundTexture, pname, value});
        }
    }
    void TP_TexParameterf(GLenum, GLenum, GLfloat) {}
    void TP_TexParameterfv(GLenum, GLenum, const GLfloat*) {}
    void TP_PixelStorei(GLenum, GLint) {}
    void TP_BindBuffer(GLenum, GLuint) {}

    struct ScopedTexParamRecording {
        explicit ScopedTexParamRecording(MobileGL::Vector<TexParamCall>& sink) {
            g_texParamCalls = &sink;
            g_texParamBoundTexture = 0;
        }
        ~ScopedTexParamRecording() { g_texParamCalls = nullptr; }
        ScopedTexParamRecording(const ScopedTexParamRecording&) = delete;
        ScopedTexParamRecording& operator=(const ScopedTexParamRecording&) = delete;
    };
} // namespace

// G9 AS A WHITE-BOX ASSERTION (gates review R1, ID-19), AND IT COVERS THE HALF THE PUBLIC-GL
// SCENARIO CANNOT.
//
// TextureParamsWithoutASamplerViewScenario catches "the parameters were EMITTED and marked
// synced but never applied": its observation is a sample, the sample creates the sampler view,
// and IsDrawSyncClean then skips the sync. What it cannot catch is a backend that merely DEFERS
// the apply to the first sampler view - the observation creates that view, the parameters land
// at that moment, and the case is green. Here the reading is taken while the texture is still
// attachment-only: nothing is ever bound to a unit, no sampler view is minted, and the
// assertion is that the parameter reached the driver ANYWAY.
//
// RED ON THE PRE-P4a BEHAVIOUR: with set_texture_params addressed by resource, this twin's
// parameter push no longer needs anything to be bound. A backend that reinstated the deferral -
// resolving the params through a sampler view, or gating the push on a unit binding - leaves
// the recording empty and this case fails, which is exactly the regression the scenario's
// self-repair hides.
TEST(DirectGLESTextureSync, AnAttachmentOnlyTexturesParametersReachTheDriverWithNoSamplerView) {
    using namespace MobileGL;
    ScopedDirectGLESTextureBindings scoped; // fresh GLContext + registry + binding caches
    Vector<TexParamCall> params;
    ScopedTexParamRecording recording(params);

    auto functions = MG_Backend::DirectGLES::g_GLESFuncs;
    functions.glBindTexture = TP_BindTexture;
    functions.glActiveTexture = TP_ActiveTexture;
    functions.glTexParameteri = TP_TexParameteri;
    functions.glTexParameterf = TP_TexParameterf;
    functions.glTexParameterfv = TP_TexParameterfv;
    functions.glPixelStorei = TP_PixelStorei;
    functions.glBindBuffer = TP_BindBuffer;
    MG_Backend::DirectGLES::SetGLESFuncsTable(functions);

    GLuint name = 0;
    MG_Impl::GLImpl::GenTextures(1, &name);
    const auto texture = MakeComplete2DTexture(name, 8);
    ASSERT_NE(texture, nullptr);
    // ATTACHMENT-ONLY from here on: the unit that specified it is released, so nothing in this
    // test ever binds this texture for sampling and nothing mints a sampler view for it.
    MG_Impl::GLImpl::BindTexture(GL_TEXTURE_2D, 0);

    // The parameter that has to travel. Red -> Green is not any texture's default, so a driver
    // that never hears about it is distinguishable from one that does.
    texture->SetSwizzleParam(TextureSwizzleParam::Red, TextureSwizzleParam::Green);

    auto& registry = MG_Backend::DirectGLES::TextureImpl::g_backendTextureObjects;
    auto& twin = registry.GetOrCreate(texture);
    if (!twin) {
        twin = MakeShared<MG_Backend::DirectGLES::TextureImpl::BackendTextureObject>();
    }
    ASSERT_NE(twin, nullptr);

#if MOBILEGL_PIPE_PUSH
    const MG_Pipe::MGPipeHandle res = registry.HandleOf(texture.get());
    MG_Pipe::MGPipeHandle builtinSampler = MG_Pipe::kMGPipeNullHandle;
    if (MG_Backend::DirectGLES::TextureResourceSubsystemEnabled()) {
        // The handle arm reads the applier, so the applier is what this probe writes - which is
        // also what makes it a white-box test rather than a scenario: no client emitter exists
        // on this tree, and the point is Espryt's behaviour given a record, not the client's.
        ASSERT_FALSE(MG_Pipe::MGPipeHandleIsNull(res));
        MG_Pipe::MGPResourceDesc desc{};
        desc.Resource = res;
        desc.Target = static_cast<Uint8>(MG_Pipe::MGPipeResourceTarget::Tex2D);
        desc.StorageKind = static_cast<Uint8>(TextureStorageType::Mipmap);
        desc.InternalFormat = static_cast<Uint32>(TextureInternalFormat::RGBA8);
        desc.Width = 8;
        desc.Height = 8;
        desc.Depth = 1;
        desc.ArrayLayers = 1;
        desc.Levels = 1;
        EXPECT_TRUE(MG_Pipe::MGPipeApplyResourceCreate(desc));

        // Every ITextureObject owns a sampler object, so the built-in sampler CSO is not
        // optional (a null one is Fatal{ProtocolCorruption} in the applier). It is a SAMPLER
        // CSO and not a sampler VIEW - the distinction this case exists for.
        auto samplerOwner = MakeShared<MG_State::GLState::SamplerObject>(0u);
        builtinSampler = MG_Pipe::MGPipeSlots().Acquire(MG_Pipe::MGPipeKind::SamplerCso,
                                                        samplerOwner->GetLifetimeId());
        ASSERT_FALSE(MG_Pipe::MGPipeHandleIsNull(builtinSampler));
        MG_Pipe::MGPSamplerDesc samplerDesc{};
        samplerDesc.Cso = builtinSampler;
        SamplerParameters samplerParams{};
        MG_Pipe::MGPipeApplyCreateSamplerState(samplerDesc, &samplerParams);

        MG_Pipe::MGPTextureParams pushed{};
        pushed.Res = res;
        pushed.BuiltinSampler = builtinSampler;
        pushed.MaxLevel = 0;
        pushed.Swizzle[0] = static_cast<Uint8>(TextureSwizzleParam::Green);
        pushed.Swizzle[1] = static_cast<Uint8>(TextureSwizzleParam::Green);
        pushed.Swizzle[2] = static_cast<Uint8>(TextureSwizzleParam::Blue);
        pushed.Swizzle[3] = static_cast<Uint8>(TextureSwizzleParam::Alpha);
        MG_Pipe::MGPipeApplySetTextureParams(pushed);

        // THE OBSERVATION IS TAKEN WHILE THE TEXTURE IS STILL ATTACHMENT-ONLY: no sampler view
        // record exists, and no sampler-view twin does either. This is the assertion the public
        // scenario cannot make, because making it there would create the view.
        EXPECT_EQ(MG_Backend::DirectGLES::SamplerViewImpl::g_backendSamplerViews.FindByHandle(
                      MG_Pipe::MGPipeSlots().FindByLifetimeId(MG_Pipe::MGPipeKind::SamplerViewCso,
                                                              texture->GetLifetimeId())),
                  nullptr)
            << "a sampler view was minted for a texture nothing sampled";
    }
#endif

    const SizeT before = params.size();
    twin->SyncTextureParamsToBackend(texture);

    Bool sawSwizzleR = false;
    for (SizeT i = before; i < params.size(); ++i) {
        if (params[i].pname == GL_TEXTURE_SWIZZLE_R) {
            sawSwizzleR = true;
            EXPECT_EQ(params[i].value, static_cast<GLint>(GL_GREEN))
                << "the swizzle reached the driver with the wrong value";
        }
    }
    EXPECT_TRUE(sawSwizzleR)
        << "an attachment-only texture's GL_TEXTURE_SWIZZLE_R never reached the driver: the "
           "parameter push is gated on something being bound, which is the deferral G9's public "
           "scenario cannot observe";

#if MOBILEGL_PIPE_PUSH
    if (!MG_Pipe::MGPipeHandleIsNull(builtinSampler)) {
        MG_Pipe::MGPipeApplyDeleteSamplerState(MG_Pipe::MGPHandleOnly{builtinSampler});
        MG_Pipe::MGPipeSlots().Free(MG_Pipe::MGPipeKind::SamplerCso, builtinSampler);
    }
#endif
}

TEST(DirectVulkanSanity, GraphicsSamplerFeedbackOnlyAliasesWritableOverlappingMip) {
    using MobileGL::MG_Backend::DirectVulkan::UniformManager;

    EXPECT_TRUE(UniformManager::SamplerOverlapsWritableImageSubresource(1, 3, 2, GL_WRITE_ONLY));
    EXPECT_TRUE(UniformManager::SamplerOverlapsWritableImageSubresource(1, 3, 3, GL_READ_WRITE));
    EXPECT_FALSE(UniformManager::SamplerOverlapsWritableImageSubresource(1, 3, 2, GL_READ_ONLY));
    EXPECT_FALSE(UniformManager::SamplerOverlapsWritableImageSubresource(1, 3, 0, GL_WRITE_ONLY));
    EXPECT_FALSE(UniformManager::SamplerOverlapsWritableImageSubresource(1, 3, 4, GL_WRITE_ONLY));
}

// GL_MAX_COMBINED_*_UNIFORM_COMPONENTS is components + blocks * (blockSize / 4). The product was
// formed in signed 32-bit, and a Vulkan host that reports a large VkPhysicalDeviceLimits::
// maxUniformBufferRange (a Mali driver answers 0xFFFFFFFF, which the loader saturates to
// INT32_MAX) made 14 * (2147483647 / 4) + 4096 wrap to -1073737742 - which is byte for byte what
// the conformance suite read back as "Limit value is: -1073737742 when it should not be smaller
// than 58368". GLES escaped it only because the ES driver answers 65536 for the block size.
TEST(GetterSanity, CombinedUniformComponentsSaturateInsteadOfOverflowing) {
    using namespace MobileGL;

    MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();

    // GL_MAX_COMBINED_COMPUTE_UNIFORM_COMPONENTS (0x8266), NOT the per-stage
    // GL_MAX_COMPUTE_UNIFORM_COMPONENTS (0x8263) this list used to name. The per-stage token is
    // answered by a frontend constant and never reaches GetMaxCombinedUniformComponents at all, so
    // both assertions on it were vacuous - and it displaced the ONE reader whose block count comes
    // from the backend (ClampUniformBlockCount(dynamicParameters.MaxComputeUniformBlocks)) rather
    // than from a frontend constant, i.e. the only call site where the saturation actually depends
    // on data a driver supplies.
    static constexpr GLenum kCombinedPnames[] = {
        GL_MAX_COMBINED_VERTEX_UNIFORM_COMPONENTS,   GL_MAX_COMBINED_FRAGMENT_UNIFORM_COMPONENTS,
        GL_MAX_COMBINED_GEOMETRY_UNIFORM_COMPONENTS, GL_MAX_COMBINED_TESS_CONTROL_UNIFORM_COMPONENTS,
        GL_MAX_COMBINED_TESS_EVALUATION_UNIFORM_COMPONENTS, GL_MAX_COMBINED_COMPUTE_UNIFORM_COMPONENTS,
    };
    // The GL 4.6 core table 23.64 floor, which all six combined pnames carry.
    static constexpr GLint kCombinedFloor = 58368;

    {
        MG_Backend::DynamicBackendParameters params;
        params.MaxUniformBlockSize = std::numeric_limits<GLint>::max();
        MG_Backend::pActiveBackendObject = MakeUnique<DynamicParameterBackend>(params);

        for (const GLenum pname: kCombinedPnames) {
            GLint reported = 0;
            MG_Impl::GLImpl::GetIntegerv(pname, &reported);
            EXPECT_GT(reported, 0) << "pname 0x" << pname << " wrapped to a negative combined component count";
            EXPECT_GE(reported, kCombinedFloor) << "pname 0x" << pname << " fell under the GL 4.6 floor";
        }
        MG_Backend::pActiveBackendObject.reset();
    }

    // An ordinary 64 KiB block size still produces the plain arithmetic, not a saturated value:
    // saturation must be the ceiling, never the answer.
    {
        MG_Backend::DynamicBackendParameters params;
        params.MaxUniformBlockSize = 65536;
        MG_Backend::pActiveBackendObject = MakeUnique<DynamicParameterBackend>(params);

        GLint reported = 0;
        MG_Impl::GLImpl::GetIntegerv(GL_MAX_COMBINED_VERTEX_UNIFORM_COMPONENTS, &reported);
        // 4096 default-block components + 14 blocks x (65536 / 4) components each.
        EXPECT_EQ(reported, 4096 + 14 * (65536 / 4));
        EXPECT_LT(reported, std::numeric_limits<GLint>::max());
        MG_Backend::pActiveBackendObject.reset();
    }

    MG_State::pGLContext.reset();
}


#if MOBILEGL_PIPE_PUSH
namespace {
    // A stand-in frontend object for the twin table. It carries the one thing the table asks of a
    // state object - GetLifetimeId() - so these cases can pin the identity contract without a
    // GLContext, a driver or a backend twin that would want ES entry points.
    struct FakeStateObject {
        explicit FakeStateObject(MobileGL::Uint64 lifetimeId): m_lifetimeId(lifetimeId) {}
        MobileGL::Uint64 GetLifetimeId() const { return m_lifetimeId; }

    private:
        MobileGL::Uint64 m_lifetimeId;
    };

    struct FakeBackendObject {
        int marker = 0;
    };

    // Kinds Query and Fence are unused by every shipping path, so these cases cannot disturb
    // the slot space any real twin table allocates out of.
    //
    // TWO of them, because MGPipeSlots() is a process-global singleton and three of the cases
    // below read its per-kind LiveCount / HighWater. The two-holder case is the one that can
    // perturb another, so it gets a kind of its own rather than a promise about gtest's
    // registration order: --gtest_shuffle, --gtest_filter and a future case are all free to
    // reorder them, and a shared kind would make that a flake.
    using FakeSlotTable = MobileGL::MG_Backend::DirectGLES::
        BackendSlotTable<FakeStateObject, FakeBackendObject, MobileGL::MG_Pipe::MGPipeKind::Query>;
    using FakeSharedKindSlotTable = MobileGL::MG_Backend::DirectGLES::
        BackendSlotTable<FakeStateObject, FakeBackendObject, MobileGL::MG_Pipe::MGPipeKind::Fence>;

    // A log file path no other process and no other case can be writing to: the pid keeps two
    // SanityTest processes on one host apart, the counter keeps two cases in one process apart.
    std::filesystem::path UniqueScratchLogPath(const char* stem) {
        static int counter = 0;
#if defined(_WIN32)
        const long pid = static_cast<long>(_getpid());
#else
        const long pid = static_cast<long>(::getpid());
#endif
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        return std::filesystem::temp_directory_path() /
               (std::string(stem) + "-" + std::to_string(pid) + "-" + std::to_string(++counter) +
                "-" + std::to_string(ticks) + ".log");
    }

    // Points MobileGL's file log at `path` for the life of the guard and puts back whatever the
    // operator had - the previous MOBILEGL_LOG_FILE_PATH, or none - on EVERY exit path,
    // including a failed ASSERT. The log is closed on both sides of the switch, because Log.cpp
    // reads the variable only when it opens the file.
    struct ScopedLogFileRedirect {
        explicit ScopedLogFileRedirect(const std::filesystem::path& path): m_path(path) {
            if (const char* previous = std::getenv("MOBILEGL_LOG_FILE_PATH")) {
                m_hadPrevious = true;
                m_previous = previous;
            }
            std::filesystem::remove(m_path);
            MobileGL::MG_Util::Debug::Close();
            SetEnvVar("MOBILEGL_LOG_FILE_PATH", m_path.string().c_str());
        }
        ~ScopedLogFileRedirect() {
            MobileGL::MG_Util::Debug::Close();
            if (m_hadPrevious) {
                SetEnvVar("MOBILEGL_LOG_FILE_PATH", m_previous.c_str());
            } else {
                UnsetEnvVar("MOBILEGL_LOG_FILE_PATH");
            }
            std::error_code ignored;
            std::filesystem::remove(m_path, ignored);
        }
        ScopedLogFileRedirect(const ScopedLogFileRedirect&) = delete;
        ScopedLogFileRedirect& operator=(const ScopedLogFileRedirect&) = delete;

        // Everything written so far. Closes the log first so the last line is on disk.
        std::string Contents() const {
            MobileGL::MG_Util::Debug::Close();
            std::ifstream logFile(m_path);
            if (!logFile.good()) return {};
            return std::string(std::istreambuf_iterator<char>(logFile), std::istreambuf_iterator<char>());
        }

    private:
        std::filesystem::path m_path;
        bool m_hadPrevious = false;
        std::string m_previous;
    };

    // Sets the two knobs into the combination that leaves no twin-table arm at all -
    // kMGPipeSubsystemEsprytSlots clear and PipeLegacyMemos false, which is what
    // MOBILEGL_PIPE_PUSH=0 MOBILEGL_PIPE_LEGACY_MEMOS=0 in the environment produces - and
    // restores MG_Config::Features on every exit path.
    struct ScopedArmlessKnobPair {
        ScopedArmlessKnobPair():
            m_savedPush(MobileGL::MG_Config::Features.PipePush),
            m_savedLegacy(MobileGL::MG_Config::Features.PipeLegacyMemos) {
            MobileGL::MG_Config::Features.PipePush =
                m_savedPush & ~MobileGL::MG_Pipe::kMGPipeSubsystemEsprytSlots;
            MobileGL::MG_Config::Features.PipeLegacyMemos = false;
        }
        ~ScopedArmlessKnobPair() {
            MobileGL::MG_Config::Features.PipePush = m_savedPush;
            MobileGL::MG_Config::Features.PipeLegacyMemos = m_savedLegacy;
        }
        ScopedArmlessKnobPair(const ScopedArmlessKnobPair&) = delete;
        ScopedArmlessKnobPair& operator=(const ScopedArmlessKnobPair&) = delete;

    private:
        MobileGL::Uint64 m_savedPush;
        MobileGL::Bool m_savedLegacy;
    };
} // namespace

// The property the whole slice exists for. The pre-P2 registry keyed twins on the frontend heap
// ADDRESS and defended the recycle with a weak_ptr; here the key is {slot, gen}, so a successor
// object landing on a slot its predecessor owned is a DIFFERENT handle, and the predecessor's
// handle resolves to nothing rather than to the successor's twin.
TEST(DirectGLESSlotTable, ARecycledSlotIsANewHandleAndTheStaleOneResolvesToNothing) {
    using namespace MobileGL;

    FakeSlotTable table;
    auto first = MakeShared<FakeStateObject>(0xA1u);
    table.GetOrCreate(first) = MakeShared<FakeBackendObject>();
    (*table.Find(first.get()))->marker = 1;

    const MG_Pipe::MGPipeHandle firstHandle = table.HandleOf(first.get());
    ASSERT_FALSE(MG_Pipe::MGPipeHandleIsNull(firstHandle));
    EXPECT_EQ(table.LiveCount(), 1u);

    // The frontend object dies and announces it (FakeStateObject is not one of the six re-keyed
    // classes, so the notice its destructor would raise is raised by hand), and the slot is
    // reclaimed - which is the only moment Gen moves.
    first.reset();
    EXPECT_TRUE(FakeSlotTable::OnFrontendObjectDestroyed(0xA1u));
    EXPECT_EQ(table.LiveCount(), 0u);
    EXPECT_EQ(table.FindByHandle(firstHandle), nullptr)
        << "a handle whose object is gone still resolved to a twin";

    auto second = MakeShared<FakeStateObject>(0xA2u);
    table.GetOrCreate(second) = MakeShared<FakeBackendObject>();
    (*table.Find(second.get()))->marker = 2;

    const MG_Pipe::MGPipeHandle secondHandle = table.HandleOf(second.get());
    EXPECT_EQ(secondHandle.Slot, firstHandle.Slot) << "the free list did not hand the slot back";
    EXPECT_NE(secondHandle.Gen, firstHandle.Gen) << "the generation did not move on slot reuse";
    EXPECT_FALSE(firstHandle == secondHandle);

    // The stale handle must not resolve to its successor's twin. This is the ABA the address key
    // could only paper over.
    EXPECT_EQ(table.FindByHandle(firstHandle), nullptr);
    ASSERT_NE(table.FindByHandle(secondHandle), nullptr);
    EXPECT_EQ((*table.FindByHandle(secondHandle))->marker, 2);

    second.reset();
    EXPECT_TRUE(FakeSlotTable::OnFrontendObjectDestroyed(0xA2u));
}

// Gen moves on reuse and ONLY on reuse: a live object that is looked up again, or respecified,
// keeps the handle it was minted with (MGPipeHandles.h).
TEST(DirectGLESSlotTable, RepeatedLookupsOfALiveObjectKeepOneHandle) {
    using namespace MobileGL;

    FakeSlotTable table;
    auto object = MakeShared<FakeStateObject>(0xB1u);
    // An object that is bound but never synced has no twin and no handle; asking is a miss.
    EXPECT_TRUE(MG_Pipe::MGPipeHandleIsNull(table.HandleOf(object.get())));
    table.GetOrCreate(object) = MakeShared<FakeBackendObject>();
    const MG_Pipe::MGPipeHandle handle = table.HandleOf(object.get());
    ASSERT_FALSE(MG_Pipe::MGPipeHandleIsNull(handle))
        << "the memo went on answering a null handle after the twin was created";

    for (int i = 0; i < 8; ++i) {
        auto* slot = table.GetOrCreate(object) ? table.Find(object.get()) : nullptr;
        ASSERT_NE(slot, nullptr);
        EXPECT_TRUE(table.HandleOf(object.get()) == handle) << "handle moved on lookup " << i;
    }
    EXPECT_EQ(table.LiveCount(), 1u);

    object.reset();
    EXPECT_TRUE(FakeSlotTable::OnFrontendObjectDestroyed(0xB1u));
}

// The round-4 review's minor 8: HandleOf used to memoise a NULL answer, on the argument that
// GetOrCreate refreshes the memo. That holds for ONE table. The allocator is per kind and the
// memo is per table, so once a second holder of the kind can be the one that acquires, the first
// table's cached "no handle" outlives the twin's creation and nothing on its own acquire path
// ever corrects it - every Find through it is a miss for a twin that exists. This is that
// configuration, and it must resolve.
TEST(DirectGLESSlotTable, ANegativeLookupIsNotCachedAcrossAnotherHoldersAcquire) {
    using namespace MobileGL;

    FakeSharedKindSlotTable first;
    FakeSharedKindSlotTable second;
    auto object = MakeShared<FakeStateObject>(0xB2u);

    // `first` asks before anyone has acquired: a miss, which must NOT be remembered.
    EXPECT_TRUE(MG_Pipe::MGPipeHandleIsNull(first.HandleOf(object.get())));

    // `second` is the holder that acquires.
    second.GetOrCreate(object) = MakeShared<FakeBackendObject>();
    const MG_Pipe::MGPipeHandle handle = second.HandleOf(object.get());
    ASSERT_FALSE(MG_Pipe::MGPipeHandleIsNull(handle));

    // `first` never acquired, so nothing on its own path refreshed its memo; it must still
    // answer the handle the kind now has for this object.
    EXPECT_TRUE(first.HandleOf(object.get()) == handle)
        << "the first holder kept answering the null handle it cached before the second holder "
           "acquired, so every lookup through it misses a twin that exists";

    object.reset();
    EXPECT_TRUE(FakeSharedKindSlotTable::OnFrontendObjectDestroyed(0xB2u));
}

// The lookup does not mutate the table, which is what lets SyncTextureObjectToBackend stop paying
// a by-value copy plus a second Find to survive the registry's erase-inside-Find. An entry whose
// object has gone stays put until the death notice arrives, and a live entry's pointer is
// unaffected by looking up anything else.
TEST(DirectGLESSlotTable, FindNeverMutatesTheTable) {
    using namespace MobileGL;

    FakeSlotTable table;
    auto kept = MakeShared<FakeStateObject>(0xC1u);
    auto doomed = MakeShared<FakeStateObject>(0xC2u);
    table.GetOrCreate(kept) = MakeShared<FakeBackendObject>();
    table.GetOrCreate(doomed) = MakeShared<FakeBackendObject>();

    auto* keptSlot = table.Find(kept.get());
    ASSERT_NE(keptSlot, nullptr);
    const FakeBackendObject* keptTwin = keptSlot->get();

    // The object goes but its notice is deliberately withheld for a moment, so that whatever
    // changes between here and the notice is Find's doing. The registry's Find would have
    // erased the expired entry here and relocated the rest of the probe cluster, invalidating
    // keptSlot. This one answers what it answers and touches nothing.
    doomed.reset();
    EXPECT_EQ(table.Find(kept.get()), keptSlot);
    EXPECT_EQ(table.LiveCount(), 2u) << "Find reclaimed a slot; only a death notice may do that";
    EXPECT_EQ(keptSlot->get(), keptTwin);

    EXPECT_TRUE(FakeSlotTable::OnFrontendObjectDestroyed(0xC2u));
    EXPECT_EQ(table.LiveCount(), 1u);
    EXPECT_EQ(table.Find(kept.get())->get(), keptTwin);

    kept.reset();
    EXPECT_TRUE(FakeSlotTable::OnFrontendObjectDestroyed(0xC1u));
}

// ScopedDirectGLESTextureBindings saves a whole twin table by value, resets it with `= {}` and
// restores it. The slot table has to keep that shape or the fixture stops isolating anything -
// and the saved copy is a HOLDER for as long as it exists, so a death announced while it is
// held reaches it too (the round-4 review's minor 2, in the fixture's own shape).
TEST(DirectGLESSlotTable, AWholeTableSavesResetsAndRestores) {
    using namespace MobileGL;

    const Uint32 holdersBefore = FakeSlotTable::HolderCount();
    FakeSlotTable table;
    auto object = MakeShared<FakeStateObject>(0xD1u);
    table.GetOrCreate(object) = MakeShared<FakeBackendObject>();
    (*table.Find(object.get()))->marker = 7;
    const MG_Pipe::MGPipeHandle handle = table.HandleOf(object.get());

    const FakeSlotTable saved = table;
    EXPECT_EQ(FakeSlotTable::HolderCount(), holdersBefore + 2u)
        << "the by-value copy did not register as a holder";
    table = {};
    EXPECT_EQ(FakeSlotTable::HolderCount(), holdersBefore + 2u)
        << "the reset changed the holder count - a temporary's registration leaked or the "
           "table's own was lost";
    EXPECT_EQ(table.Find(object.get()), nullptr) << "the reset left the twin reachable";

    table = saved;
    ASSERT_NE(table.Find(object.get()), nullptr);
    EXPECT_EQ((*table.Find(object.get()))->marker, 7);

    // The object dies while BOTH the working table and the saved copy hold its twin. One
    // notice, and neither may keep a live entry.
    object.reset();
    EXPECT_TRUE(FakeSlotTable::OnFrontendObjectDestroyed(0xD1u));
    EXPECT_EQ(table.FindByHandle(handle), nullptr);
    EXPECT_EQ(table.LiveCount(), 0u);
    EXPECT_EQ(saved.LiveCount(), 0u)
        << "the saved copy kept the dead object's twin - the notice reached one holder only";
}

// The whole point of routing every twin through the client allocator: a table that keeps its own
// dense array still shares ONE identity per frontend object with every other holder of it.
//
// Comparing a.HandleOf(o) with b.HandleOf(o) alone would prove nothing - HandleOf never reads the
// table, so any two tables agree for any implementation. What is actually load-bearing, and what
// is asserted here, is that ONE slot of the kind is consumed for the object no matter how many
// tables hold a twin of it (a per-table allocator would pass the pure compare and fail this), and
// that the shared handle still addresses each table's OWN twin.
TEST(DirectGLESSlotTable, TwoTablesOfTheSameKindShareOneSlotAndKeepTheirOwnTwin) {
    using namespace MobileGL;

    constexpr MG_Pipe::MGPipeKind kKind = MG_Pipe::MGPipeKind::Fence;
    auto& slots = MG_Pipe::MGPipeSlots();
    const Uint32 liveBefore = slots.LiveCount(kKind);

    FakeSharedKindSlotTable a;
    FakeSharedKindSlotTable b;
    auto object = MakeShared<FakeStateObject>(0xE1u);
    a.GetOrCreate(object) = MakeShared<FakeBackendObject>();
    (*a.Find(object.get()))->marker = 1;
    b.GetOrCreate(object) = MakeShared<FakeBackendObject>();
    (*b.Find(object.get()))->marker = 2;

    EXPECT_EQ(slots.LiveCount(kKind), liveBefore + 1u)
        << "the two tables minted a slot each; Magma's table would then resolve a different "
           "handle for the same object than Espryt's";

    const MG_Pipe::MGPipeHandle handle = a.HandleOf(object.get());
    ASSERT_FALSE(MG_Pipe::MGPipeHandleIsNull(handle));
    EXPECT_TRUE(b.HandleOf(object.get()) == handle);

    ASSERT_NE(a.FindByHandle(handle), nullptr);
    ASSERT_NE(b.FindByHandle(handle), nullptr);
    EXPECT_EQ((*a.FindByHandle(handle))->marker, 1);
    EXPECT_EQ((*b.FindByHandle(handle))->marker, 2);

    // TWO HOLDERS OF ONE SLOT is a real configuration, not a test artefact (the
    // ScopedDirectGLESTextureBindings fixture above holds a second live table of kind Texture
    // for the length of a test), and the death of the object is what makes it sharp: the
    // allocator forgets the lifetime id on Free, so a notice delivered to ONE holder leaves
    // the other with a live entry - and its twin's driver storage - that nothing can resolve
    // and nothing sweeps. OneDeathNoticeDropsTheTwinInEveryHolderOfTheKind below is the case
    // for that; here the cleanup only has to leave the kind's LiveCount where it was.
    object.reset();
    EXPECT_TRUE(FakeSharedKindSlotTable::OnFrontendObjectDestroyed(0xE1u));
    EXPECT_EQ(slots.LiveCount(kKind), liveBefore)
        << "the shared slot outlived both holders and the object";
}

// The round-4 review's minor 2, closed: one notice, EVERY holder. Before this the dispatcher
// told one table per kind and DestroyByLifetimeId freed only a slot THIS table held, so with
// the sweep retired the second holder kept a live entry, and the twin, for the life of the
// process. Three holders here - two independent tables and a by-value copy, which is exactly
// what the fixture makes - and a fourth that never twinned the object and must be untouched.
TEST(DirectGLESSlotTable, OneDeathNoticeDropsTheTwinInEveryHolderOfTheKind) {
    using namespace MobileGL;

    constexpr MG_Pipe::MGPipeKind kKind = MG_Pipe::MGPipeKind::Fence;
    auto& slots = MG_Pipe::MGPipeSlots();
    const Uint32 liveBefore = slots.LiveCount(kKind);
    const Uint32 holdersBefore = FakeSharedKindSlotTable::HolderCount();

    FakeSharedKindSlotTable a;
    FakeSharedKindSlotTable b;
    FakeSharedKindSlotTable bystander;
    auto object = MakeShared<FakeStateObject>(0xE2u);
    auto uninvolved = MakeShared<FakeStateObject>(0xE3u);

    a.GetOrCreate(object) = MakeShared<FakeBackendObject>();
    b.GetOrCreate(object) = MakeShared<FakeBackendObject>();
    bystander.GetOrCreate(uninvolved) = MakeShared<FakeBackendObject>();
    const FakeSharedKindSlotTable copyOfA = a; // the fixture's saved registry
    EXPECT_EQ(FakeSharedKindSlotTable::HolderCount(), holdersBefore + 4u);

    const MG_Pipe::MGPipeHandle handle = a.HandleOf(object.get());
    ASSERT_FALSE(MG_Pipe::MGPipeHandleIsNull(handle));
    // Observers on the twins, so "dropped" means destroyed and not merely unreachable.
    const std::weak_ptr<FakeBackendObject> twinA = *a.FindByHandle(handle);
    const std::weak_ptr<FakeBackendObject> twinB = *b.FindByHandle(handle);
    EXPECT_EQ(slots.LiveCount(kKind), liveBefore + 2u);

    // ONE notice. The object is still alive so that nothing but the notice can be at work.
    EXPECT_TRUE(FakeSharedKindSlotTable::OnFrontendObjectDestroyed(object->GetLifetimeId()));

    EXPECT_EQ(a.FindByHandle(handle), nullptr) << "holder a kept the twin";
    EXPECT_EQ(b.FindByHandle(handle), nullptr) << "holder b kept the twin";
    EXPECT_EQ(copyOfA.LiveCount(), 0u) << "the by-value copy kept the twin";
    EXPECT_EQ(a.LiveCount(), 0u);
    EXPECT_EQ(b.LiveCount(), 0u);
    EXPECT_TRUE(twinA.expired()) << "a's twin is unreachable but still allocated";
    EXPECT_TRUE(twinB.expired()) << "b's twin is unreachable but still allocated";
    EXPECT_EQ(slots.LiveCount(kKind), liveBefore + 1u) << "the slot was not returned exactly once";
    EXPECT_FALSE(FakeSharedKindSlotTable::OnFrontendObjectDestroyed(object->GetLifetimeId()))
        << "a second notice for the same object found a slot to free";

    // The holder that never twinned the object is exactly as it was.
    EXPECT_EQ(bystander.LiveCount(), 1u);
    ASSERT_NE(bystander.Find(uninvolved.get()), nullptr);

    object.reset();
    uninvolved.reset();
    EXPECT_TRUE(FakeSharedKindSlotTable::OnFrontendObjectDestroyed(0xE3u));
    EXPECT_EQ(bystander.LiveCount(), 0u);
    EXPECT_EQ(slots.LiveCount(kKind), liveBefore);
}

// The sweep and both of its drivers are RETIRED on this arm (ROADMAP.md:18's "delete the GC"),
// and this is the property that replaces them. The registry this table replaces learned of a
// death only by finding an expired weak_ptr, so it needed a 1024-call draw tick AND a
// 64-creation tick and still held up to 64 dead, gigabyte-sized twins at once. An announced
// death returns the slot before the next creation asks for one, so NOTHING accumulates -
// nothing below calls CollectGarbageIfNeeded() or CollectGarbageNow(), and on this arm the
// former does nothing at all.
TEST(DirectGLESSlotTable, AnnouncedDeathKeepsObjectChurnFromAccumulatingWithoutASweep) {
    using namespace MobileGL;

    auto& slots = MG_Pipe::MGPipeSlots();
    constexpr Uint32 kChurn = 256u;
    const Uint32 highWaterBefore = slots.HighWater(MG_Pipe::MGPipeKind::Query);
    const Uint32 liveBefore = slots.LiveCount(MG_Pipe::MGPipeKind::Query);

    FakeSlotTable table;
    Uint32 peakLive = 0;
    for (Uint32 i = 0; i < kChurn; ++i) {
        auto object = MakeShared<FakeStateObject>(0xF0000000ull + i);
        table.GetOrCreate(object) = MakeShared<FakeBackendObject>();
        peakLive = std::max(peakLive, table.LiveCount());
        // What a real object's destructor raises. FakeStateObject is not one of the six
        // re-keyed frontend classes, so the firing half is driven by hand here; that those six
        // classes really do fire it is EveryReKeyedObjectClassAnnouncesItsOwnDeath below, and
        // that the registries answer it per kind is
        // EverySwitchedOverKindResolvesItsTwinThroughTheHandleArm.
        EXPECT_TRUE(FakeSlotTable::OnFrontendObjectDestroyed(object->GetLifetimeId()));
    }

    EXPECT_EQ(peakLive, 1u)
        << peakLive << " twins were live at once with " << kChurn
        << " objects churned and every death announced - the notice stopped freeing the twin";
    EXPECT_EQ(table.LiveCount(), 0u);
    EXPECT_EQ(slots.LiveCount(MG_Pipe::MGPipeKind::Query), liveBefore)
        << "the churn leaked slots the announced deaths should have returned";
    // 2 and not 1: on a cold allocator the high-water mark counts the RESERVED slot 0
    // (kMGPipeFirstAllocatableSlot is 1) as well as the one slot this loop recycles, and ctest
    // runs every case in its own process, so this case sees a cold allocator. What the bound
    // rules out is the thing that matters - 256 churned objects growing the space by 256.
    EXPECT_LE(slots.HighWater(MG_Pipe::MGPipeKind::Query) - highWaterBefore, 2u)
        << "the slot space grew with the churn instead of being recycled";
}

// The map arm inserted a null key and handed back that entry's twin, and
// SyncTextureObjectToBackend documents relying on it. Release builds compile the assert out, so
// on the handle arm this has to be a defined answer rather than a dereference of null.
TEST(DirectGLESSlotTable, GetOrCreateToleratesANullStateObject) {
    using namespace MobileGL;

    FakeSlotTable table;
    const SharedPtr<FakeStateObject> none;
    auto& twin = table.GetOrCreate(none);
    EXPECT_EQ(twin, nullptr);
    EXPECT_EQ(table.LiveCount(), 0u) << "a null object took a slot";
    EXPECT_EQ(table.Find(nullptr), nullptr);
    EXPECT_TRUE(MG_Pipe::MGPipeHandleIsNull(table.HandleOf(nullptr)));

    // ...and a SECOND null call is handed the same parking slot rather than destroying what the
    // first one was given. The map arm kept its null-keyed entry until a sweep, so a table that
    // reset here would answer differently on the two arms in the one path that documents
    // relying on this tolerance.
    twin = MakeShared<FakeBackendObject>();
    twin->marker = 5;
    auto& again = table.GetOrCreate(none);
    ASSERT_NE(again, nullptr) << "the second null call destroyed the first one's parked twin";
    EXPECT_EQ(again->marker, 5);
    EXPECT_EQ(&again, &twin);
    EXPECT_EQ(table.LiveCount(), 0u);
}
// P2 step e2, the backend half. A sweep is a stand-in for a death notice; this is the notice.
// Nothing below calls CollectGarbage*: the slot comes back, and the twin goes, at the moment
// the frontend says the object is gone - which for a texture atlas or a renderbuffer is the
// difference between freeing a driver allocation now and freeing it 64 creations from now.
TEST(DirectGLESSlotTable, AnAnnouncedDeathReturnsTheSlotWithoutASweep) {
    using namespace MobileGL;

    auto& slots = MG_Pipe::MGPipeSlots();
    const Uint32 liveBefore = slots.LiveCount(MG_Pipe::MGPipeKind::Query);

    FakeSlotTable table;
    auto object = MakeShared<FakeStateObject>(0x1E2A0001ull);
    table.GetOrCreate(object) = MakeShared<FakeBackendObject>();
    const MG_Pipe::MGPipeHandle handle = table.HandleOf(object.get());
    ASSERT_FALSE(MG_Pipe::MGPipeHandleIsNull(handle));
    EXPECT_EQ(slots.LiveCount(MG_Pipe::MGPipeKind::Query), liveBefore + 1u);

    // The object is STILL ALIVE here, which is the point: there is no weak_ptr test anywhere
    // that could fire, so anything that changes is the notice's doing and nothing else's.
    EXPECT_TRUE(FakeSlotTable::OnFrontendObjectDestroyed(object->GetLifetimeId()));
    EXPECT_EQ(table.LiveCount(), 0u) << "the twin survived its own destroy notice";
    EXPECT_EQ(table.FindByHandle(handle), nullptr);
    EXPECT_EQ(table.Find(object.get()), nullptr);
    EXPECT_EQ(slots.LiveCount(MG_Pipe::MGPipeKind::Query), liveBefore)
        << "the slot was not returned to the allocator";

    // Idempotent: the allocator no longer maps the id, so a repeated notice frees nothing and
    // says so - and it says so for every holder, since the notice is about the object and not
    // about a table.
    EXPECT_FALSE(FakeSlotTable::OnFrontendObjectDestroyed(object->GetLifetimeId()));

    // A slot minted for an object that NO table holds any more - the fixture's `table = {}`
    // drops entries without freeing - still goes back when the object dies: the id is dead and
    // cannot be acquired again, so keeping the slot would be the process-lifetime leak the
    // review named.
    auto orphaned = MakeShared<FakeStateObject>(0x1E2A0002ull);
    {
        FakeSlotTable transient;
        transient.GetOrCreate(orphaned) = MakeShared<FakeBackendObject>();
    }
    EXPECT_EQ(slots.LiveCount(MG_Pipe::MGPipeKind::Query), liveBefore + 1u);
    orphaned.reset();
    EXPECT_TRUE(FakeSlotTable::OnFrontendObjectDestroyed(0x1E2A0002ull))
        << "a slot no holder had an entry for was left allocated";
    EXPECT_EQ(slots.LiveCount(MG_Pipe::MGPipeKind::Query), liveBefore);
}

// The firing side of e2, on ALL SIX re-keyed object classes - the round-3 review's MAJOR 3.
// Until this round only ProgramObject and RenderbufferObject raised the notice and the other
// four discovered their death in a sweep; the sweep is now retired, so a class that stopped
// announcing would leak its twin and the driver storage that twin owns for the life of the
// process. The notice has to arrive when the LAST SharedPtr drops - not when glDelete* marks
// the name, because a still-bound object goes on living - so the objects are simply dropped.
TEST(DirectGLESSlotTable, EveryReKeyedObjectClassAnnouncesItsOwnDeath) {
    using namespace MobileGL;

    static Vector<std::pair<MG_Pipe::MGPipeKind, Uint64>> notices;
    notices.clear();
    const MG_State::GLState::StateObjectDeathOps recording = {
        .OnDestroyed = [](MG_Pipe::MGPipeKind kind, Uint64 lifetimeId) {
            notices.emplace_back(kind, lifetimeId);
        },
    };
    const MG_State::GLState::StateObjectDeathOps* previous =
        MG_State::GLState::GetStateObjectDeathOps();
    MG_State::GLState::SetStateObjectDeathOps(&recording);

    Vector<std::pair<MG_Pipe::MGPipeKind, Uint64>> expected;
    Uint64 bufferLifetimeId = 0;
    {
        auto program = MakeShared<MG_State::GLState::ProgramObject>(0u);
        auto renderbuffer = MakeShared<MG_State::GLState::RenderbufferObject>(0u);
        auto texture = MakeShared<MG_State::GLState::TextureObject2D>(0u);
        auto framebuffer = MakeShared<MG_State::GLState::FramebufferObject>(1u);
        auto sampler = MakeShared<MG_State::GLState::SamplerObject>(0u);
        auto vertexArray = MakeShared<MG_State::GLState::VertexArrayObject>(0u);

        expected.emplace_back(MG_Pipe::MGPipeKind::ShaderCso, program->GetLifetimeId());
        expected.emplace_back(MG_Pipe::MGPipeKind::Renderbuffer, renderbuffer->GetLifetimeId());
        expected.emplace_back(MG_Pipe::MGPipeKind::Texture, texture->GetLifetimeId());
        expected.emplace_back(MG_Pipe::MGPipeKind::Framebuffer, framebuffer->GetLifetimeId());
        expected.emplace_back(MG_Pipe::MGPipeKind::SamplerCso, sampler->GetLifetimeId());
        expected.emplace_back(MG_Pipe::MGPipeKind::VertexElementsCso, vertexArray->GetLifetimeId());

        // P3a, and it is DELIBERATELY NOT in `expected`: the buffer is the seventh re-keyed
        // class and the only one whose death does not travel on this notice. The catalogue
        // has a call for it - resource_destroy - so no seventh NotifyStateObjectDestroyed
        // raiser was added (D-L). StateObjectDeathNotice.h exists for kinds that have NO
        // such call, and a buffer raising one too would be two death signals for one object,
        // i.e. a slot freed twice and a successor's twin dropped under it.
        auto bufferObject = MakeShared<MG_State::GLState::BufferObject>(0u);
        bufferLifetimeId = bufferObject->GetLifetimeId();

        EXPECT_TRUE(notices.empty()) << "a live object announced its own death";
    }

    MG_State::GLState::SetStateObjectDeathOps(previous);

    EXPECT_EQ(std::find(notices.begin(), notices.end(),
                        std::make_pair(MG_Pipe::MGPipeKind::Buffer, bufferLifetimeId)),
              notices.end())
        << "a BufferObject raised a state-object death notice; P3a routes buffer death through "
           "resource_destroy instead (D-L), and both firing would free the slot twice";

    // Membership rather than a count or an order: every TextureObjectBase owns a private
    // SamplerObject (TextureObject.cpp), so tearing a texture down legitimately raises a
    // SamplerCso notice as well. What must hold is that each of the six classes announced its
    // OWN id under its OWN kind.
    for (const auto& want : expected) {
        EXPECT_NE(std::find(notices.begin(), notices.end(), want), notices.end())
            << "kind " << static_cast<Uint32>(want.first) << " lifetime id " << want.second
            << " was destroyed without announcing it, so its twin would wait for a sweep that "
               "this arm no longer runs";
    }
}

// ... and that the backend actually installs a consumer for it, rather than the two halves
// each being fine on their own. Registered from ResolveEsprytSlotTablesArm(), i.e. exactly
// when the arm that can answer a notice is the arm that runs.
TEST(DirectGLESSlotTable, TheHandleArmInstallsTheDeathNoticeConsumer) {
    using namespace MobileGL;

    if (!MG_Backend::DirectGLES::EsprytSlotTablesEnabled()) {
        GTEST_SKIP() << "the legacy arm keys on the frontend address and cannot answer a notice";
    }
    ASSERT_NE(MG_State::GLState::GetStateObjectDeathOps(), nullptr)
        << "the handle arm runs but nothing consumes a death notice, so every twin still waits "
           "for a garbage sweep";
    EXPECT_NE(MG_State::GLState::GetStateObjectDeathOps()->OnDestroyed, nullptr);
}

// The gate on MAJOR 1 of the round-2 review: this binary's OTHER 82 cases - among them every
// D13 "must not break" item - are only evidence about this package if they run on the arm this
// package wrote. Before EsprytSlotArmEnvironment existed they did not, in any build directory
// the P2 brief defines, and nothing said so; a gdb breakpoint on MGPipeSlotAllocator::Acquire
// was the only way to find out. This case is that breakpoint, made falsifiable: delete the
// environment and it goes red, and it goes red naming the arm rather than the symptom.
TEST(DirectGLESSlotTable, TheTwinRegistryCasesInThisBinaryRunOnTheHandleArm) {
    const char* knob = std::getenv("MOBILEGL_PIPE_PUSH");
    if (knob != nullptr && *knob != '\0') {
        GTEST_SKIP() << "the operator pinned the arm with MOBILEGL_PIPE_PUSH=" << knob;
    }
    EXPECT_TRUE(MobileGL::MG_Backend::DirectGLES::EsprytSlotTablesEnabled())
        << "a push build of SanityTest resolved the LEGACY twin registry, so every case in this "
           "binary that builds a twin - the scratch-FBO scrub, the three context-generation "
           "guards, the sampled-set staleness walk, ScopedDirectGLESTextureBindings - is "
           "exercising code this package did not change";
    EXPECT_NE(MobileGL::MG_Config::Features.PipePush & MobileGL::MG_Pipe::kMGPipeSubsystemEsprytSlots,
              0ull);
}

namespace {
    // One kind's worth of the walk the re-key exists for - acquire, look up by object, look up
    // by handle, delete, re-acquire - driven through the REAL registry global that every
    // shipping path uses, and therefore through StateBackendObjectRegistry's arm dispatch.
    //
    // That "through the real registry" is the whole point of this helper. The eleven
    // DirectGLESSlotTable cases above drive BackendSlotTable directly on the throwaway kinds
    // Query and Fence, so they would pass unchanged had the six registries never been re-keyed;
    // and of the D13 "must not break" cases only the sampled-set staleness walk makes a twin at
    // all, all four of them of kind Texture. Five of the six re-keyed kinds therefore had no
    // case that could go red for the switch-over. This is that case.
    //
    // No backend twin is constructed: every one of the six twin classes generates a driver id
    // in its constructor, and none of that is what was re-keyed. What is asserted instead is
    // that GetOrCreate, Find(object) and FindByHandle(handle) all name the SAME twin storage,
    // that the handle is a real {slot, gen} rather than the null handle the legacy arm answers,
    // and that a successor object landing on the freed slot gets a different Gen while the
    // predecessor's handle resolves to nothing.
    template <typename Registry, typename MakeObject>
    void ExpectTheHandleArmDrivesThisKind(const char* kindName, Registry& registry, MakeObject make) {
        using namespace MobileGL;

        auto first = make();
        ASSERT_NE(first, nullptr) << kindName;
        auto& firstTwin = registry.GetOrCreate(first);
        ASSERT_NE(MG_State::GLState::GetStateObjectDeathOps(), nullptr)
            << kindName << ": twinning an object did not install the death-notice consumer";

        const MG_Pipe::MGPipeHandle firstHandle = registry.HandleOf(first.get());
        ASSERT_FALSE(MG_Pipe::MGPipeHandleIsNull(firstHandle))
            << kindName << ": GetOrCreate on the real registry minted no handle, so this kind is "
                           "not running on the {slot, gen} arm at all";
        EXPECT_EQ(registry.Find(first.get()), &firstTwin)
            << kindName << ": Find resolved a different twin slot than GetOrCreate handed back";
        EXPECT_EQ(registry.FindByHandle(firstHandle), &firstTwin)
            << kindName << ": the handle does not address the twin GetOrCreate handed back";

        // The frontend object dies. NOTHING below sweeps - the destructor's own notice is the
        // only thing that can free the slot, which is what makes this the e2/e3 pair end to end.
        const Uint64 firstLifetimeId = first->GetLifetimeId();
        first.reset();
        EXPECT_EQ(registry.FindByHandle(firstHandle), nullptr)
            << kindName << ": the twin outlived the announced death of its object";
        EXPECT_FALSE(registry.DestroyByLifetimeId(firstLifetimeId))
            << kindName << ": the slot was still held after its object announced its death";

        auto second = make();
        ASSERT_NE(second, nullptr) << kindName;
        auto& secondTwin = registry.GetOrCreate(second);
        const MG_Pipe::MGPipeHandle secondHandle = registry.HandleOf(second.get());
        ASSERT_FALSE(MG_Pipe::MGPipeHandleIsNull(secondHandle)) << kindName;
        EXPECT_EQ(secondHandle.Slot, firstHandle.Slot)
            << kindName << ": the freed slot was not handed back, so this walk did not exercise "
                           "the recycle it exists to test";
        EXPECT_NE(secondHandle.Gen, firstHandle.Gen)
            << kindName << ": Gen did not move on slot reuse - the predecessor's handle would "
                           "resolve to the successor's twin, which is the ABA the address key "
                           "could only paper over";
        EXPECT_EQ(registry.FindByHandle(firstHandle), nullptr)
            << kindName << ": the STALE handle resolved to a twin";
        EXPECT_EQ(registry.FindByHandle(secondHandle), &secondTwin) << kindName;

        second.reset();
        EXPECT_EQ(registry.FindByHandle(secondHandle), nullptr) << kindName;
    }
} // namespace

// The gate on MAJOR 2 of the round-3 review: every kind this package re-keyed, exercised
// through the registry the shipping code calls, on the arm this package wrote.
TEST(DirectGLESSlotTable, EverySwitchedOverKindResolvesItsTwinThroughTheHandleArm) {
    using namespace MobileGL;
    using namespace MobileGL::MG_Backend::DirectGLES;
    using namespace MobileGL::MG_State::GLState;

    if (!EsprytSlotTablesEnabled()) {
        GTEST_SKIP() << "the legacy arm keys twins on the frontend heap address and answers the "
                        "null handle, so there is no {slot, gen} walk to drive";
    }

    ExpectTheHandleArmDrivesThisKind("Texture", TextureImpl::g_backendTextureObjects, [] {
        return SharedPtr<ITextureObject>(MakeShared<TextureObject2D>(0u));
    });
    ExpectTheHandleArmDrivesThisKind("Framebuffer", FramebufferImpl::g_backendFramebufferObjects,
                                     [] { return MakeShared<FramebufferObject>(1u); });
    ExpectTheHandleArmDrivesThisKind("Renderbuffer", RenderbufferImpl::g_backendRenderbufferObjects,
                                     [] { return MakeShared<RenderbufferObject>(0u); });
    ExpectTheHandleArmDrivesThisKind("SamplerCso", SamplerImpl::g_backendSamplerObjects,
                                     [] { return MakeShared<SamplerObject>(0u); });
    ExpectTheHandleArmDrivesThisKind("ShaderCso", PrgramImpl::g_backendProgramObjects,
                                     [] { return MakeShared<ProgramObject>(0u); });
    ExpectTheHandleArmDrivesThisKind("VertexElementsCso", VertexArrayImpl::g_backendVertexArrayObjects,
                                     [] { return MakeShared<VertexArrayObject>(0u); });

    // P3a: the SEVENTH table, and the only one the helper above cannot drive - which is the
    // point of it. Every other kind is a TwinRegistry whose GetOrCreate MINTS the handle off a
    // frontend object's lifetime id from inside MG_Backend; this one is a bare BackendSlotTable
    // that only ever receives a handle the CALL carried, so there is no HandleOf, no
    // Find(object), no stateRef and no death notice. Its walk is the family's own:
    // client mints -> backend twins by handle -> resource_destroy retires the twin -> the
    // CLIENT frees the slot, in that order (D-L), and a successor at the recycled slot gets a
    // Gen that leaves the predecessor's handle resolving to nothing.
    {
        using MobileGL::MG_Backend::DirectGLES::BufferImpl::g_backendBufferResources;
        using MobileGL::MG_Backend::DirectGLES::BufferImpl::GLESBufferResource;
        auto& table = g_backendBufferResources;

        auto owner = MakeShared<BufferObject>(0u);
        const MG_Pipe::MGPipeHandle first =
            MG_Pipe::MGPipeSlots().Acquire(MG_Pipe::MGPipeKind::Buffer, owner->GetLifetimeId());
        ASSERT_FALSE(MG_Pipe::MGPipeHandleIsNull(first))
            << "Buffer: the client allocator minted no handle for a live buffer";
        auto& firstTwin = table.GetOrCreate(first);
        firstTwin = MakeShared<GLESBufferResource>();
        const GLESBufferResource* const firstRaw = firstTwin.get();
        EXPECT_EQ(table.FindByHandle(first), &firstTwin)
            << "Buffer: the handle does not address the twin GetOrCreate handed back";

        // resource_destroy: the twin comes OUT first (so the pool / delete / deferred-release
        // decision is reached with the entry already retired), and only then does the client
        // free the slot - the allocator forgets the lifetime id on Free.
        const SharedPtr<GLESBufferResource> released = table.ReleaseByHandle(first);
        EXPECT_NE(released, nullptr) << "Buffer: resource_destroy found no twin to retire";
        EXPECT_EQ(table.FindByHandle(first), nullptr)
            << "Buffer: the twin outlived its resource_destroy";
        MG_Pipe::MGPipeSlots().Free(MG_Pipe::MGPipeKind::Buffer, first);

        auto successor = MakeShared<BufferObject>(0u);
        const MG_Pipe::MGPipeHandle second =
            MG_Pipe::MGPipeSlots().Acquire(MG_Pipe::MGPipeKind::Buffer, successor->GetLifetimeId());
        ASSERT_FALSE(MG_Pipe::MGPipeHandleIsNull(second)) << "Buffer";
        EXPECT_EQ(second.Slot, first.Slot)
            << "Buffer: the freed slot was not handed back, so this walk did not exercise the "
               "recycle it exists to test";
        EXPECT_NE(second.Gen, first.Gen)
            << "Buffer: Gen did not move on slot reuse - the predecessor's handle would resolve "
               "to the successor's storage, which is the ABA the {slot, gen} key exists to stop";
        EXPECT_EQ(table.FindByHandle(first), nullptr) << "Buffer: the STALE handle resolved to a twin";

        auto& secondTwin = table.GetOrCreate(second);
        EXPECT_EQ(secondTwin, nullptr)
            << "Buffer: a resource at a recycled slot inherited its predecessor's backend storage";
        secondTwin = MakeShared<GLESBufferResource>();
        EXPECT_EQ(table.FindByHandle(second), &secondTwin) << "Buffer";
        EXPECT_NE(table.FindByHandle(second)->get(), firstRaw) << "Buffer";

        table.ReleaseByHandle(second);
        MG_Pipe::MGPipeSlots().Free(MG_Pipe::MGPipeKind::Buffer, second);
    }

    // P4a: the EIGHTH table and the SIXTH kind - the sampler view, which is the only kind in
    // the phase with no frontend object at all. MobileGL has no sampler-view class: GL binds a
    // texture to a unit and the sampler uniform's type plus the two completeness predicates
    // decide what the shader sees, gallium's one-view-per-slot IS that resolved form, and the
    // resolution moves to the CLIENT (D-F3). So this table is handle-keyed only, like the
    // buffer resource table above, and its twin owns no driver id whatsoever - what it holds is
    // the server's memo of one resolved view plus the raw-depth-fetch substitution decision,
    // which is one of the two backend post-processings ARCHITECTURE.md:206 keeps on the server.
    //
    // The handle is minted off the TEXTURE's lifetime id (D-F2: one view per ITextureObject),
    // which is what makes HandleOf resolve at all - and it is legal precisely because the two
    // kinds have separate slot spaces, so the same lifetime id names a Texture slot and a
    // SamplerViewCso slot without either shadowing the other.
    {
        using MobileGL::MG_Backend::DirectGLES::SamplerViewImpl::g_backendSamplerViews;
        using MobileGL::MG_Backend::DirectGLES::SamplerViewImpl::BackendSamplerViewObject;
        auto& table = g_backendSamplerViews;

        SharedPtr<ITextureObject> owner = MakeShared<TextureObject2D>(0u);
        const MG_Pipe::MGPipeHandle first =
            MG_Pipe::MGPipeSlots().Acquire(MG_Pipe::MGPipeKind::SamplerViewCso, owner->GetLifetimeId());
        ASSERT_FALSE(MG_Pipe::MGPipeHandleIsNull(first))
            << "SamplerViewCso: the client allocator minted no handle for a live texture's view";

        // The same lifetime id, two kinds, two independent slot spaces.
        const MG_Pipe::MGPipeHandle textureHandle =
            MG_Pipe::MGPipeSlots().FindByLifetimeId(MG_Pipe::MGPipeKind::Texture, owner->GetLifetimeId());
        EXPECT_FALSE(first == textureHandle)
            << "SamplerViewCso: the view handle and the texture handle are the same {slot, gen}, so "
               "one kind's slot space is aliasing the other's";

        auto& firstTwin = table.GetOrCreate(first);
        firstTwin = MakeShared<BackendSamplerViewObject>();
        firstTwin->SyncedSerial = 0xABCDEFull;
        const BackendSamplerViewObject* const firstRaw = firstTwin.get();
        EXPECT_EQ(table.FindByHandle(first), &firstTwin)
            << "SamplerViewCso: the handle does not address the twin GetOrCreate handed back";
        EXPECT_EQ(table.HandleOf(owner.get()), first)
            << "SamplerViewCso: HandleOf did not resolve the view minted off this texture's "
               "lifetime id, so a backend path that still arrives holding the object cannot find "
               "its view";

        const SharedPtr<BackendSamplerViewObject> released = table.ReleaseByHandle(first);
        EXPECT_NE(released, nullptr) << "SamplerViewCso: delete_sampler_view found no twin to retire";
        EXPECT_EQ(table.FindByHandle(first), nullptr)
            << "SamplerViewCso: the twin outlived its delete_sampler_view";
        MG_Pipe::MGPipeSlots().Free(MG_Pipe::MGPipeKind::SamplerViewCso, first);

        auto successorOwner = MakeShared<TextureObject2D>(0u);
        const MG_Pipe::MGPipeHandle second = MG_Pipe::MGPipeSlots().Acquire(
            MG_Pipe::MGPipeKind::SamplerViewCso, successorOwner->GetLifetimeId());
        ASSERT_FALSE(MG_Pipe::MGPipeHandleIsNull(second)) << "SamplerViewCso";
        EXPECT_EQ(second.Slot, first.Slot)
            << "SamplerViewCso: the freed slot was not handed back, so this walk did not exercise "
               "the recycle it exists to test";
        EXPECT_NE(second.Gen, first.Gen)
            << "SamplerViewCso: Gen did not move on slot reuse - the predecessor's handle would "
               "resolve to the successor's view, which is the ABA the {slot, gen} key exists to stop";
        EXPECT_EQ(table.FindByHandle(first), nullptr) << "SamplerViewCso: the STALE handle resolved";

        auto& secondTwin = table.GetOrCreate(second);
        EXPECT_EQ(secondTwin, nullptr)
            << "SamplerViewCso: a view at a recycled slot inherited its predecessor's memo, so the "
               "raw-depth-fetch decision of a dead texture would be replayed for a live one";
        secondTwin = MakeShared<BackendSamplerViewObject>();
        EXPECT_NE(table.FindByHandle(second)->get(), firstRaw) << "SamplerViewCso";

        // BACKWARD generations are REFUSED rather than adopted, exactly as they are on every
        // other handle-keyed table (SlotTables.h:301-321). Forward is a recycle; adopting a
        // backward one would retire the incumbent LIVE twin and then stamp the slot back to the
        // dead view's generation.
        //
        // m-8: THE FIRST ASSERTION BELOW CANNOT DISTINGUISH THE TWO OUTCOMES and its message
        // must not claim it does. GetOrCreate(handle) calls entry.backend.reset() before it
        // stamps, so an ADOPTED backward generation yields a null BackendPtr& just as a refused
        // one does. What separates refusal from adoption is the SECOND assertion: after an
        // adoption the live twin at `second` is gone. The first is kept because a non-null there
        // would mean the table handed back the incumbent's own twin under the dead handle, which
        // is a third outcome and a worse one.
        auto& stale = table.GetOrCreate(first);
        EXPECT_EQ(stale, nullptr)
            << "SamplerViewCso: a stale handle was answered with the LIVE twin at its slot (this "
               "assertion cannot tell a refusal from an adoption - the next one does)";
        EXPECT_NE(table.FindByHandle(second), nullptr)
            << "SamplerViewCso: the live twin was destroyed by a handle from its slot's past - "
               "the backward generation was ADOPTED rather than refused";

        table.ReleaseByHandle(second);
        MG_Pipe::MGPipeSlots().Free(MG_Pipe::MGPipeKind::SamplerViewCso, second);
    }
}

// P4a (ID-8): the backend death notice is the REDUNDANT SECOND PATH for every kind the client
// mints, and it must be IDEMPOTENT - the client's own helper emits the wire delete, raises this
// notice and frees the slot, in that order, so whichever of the two frees first wins and the
// other must find nothing and do nothing.
//
// Driven through the REAL consumer the backend installed rather than through a recording stub:
// what is under test is Espryt's OnFrontendStateObjectDestroyed arm, including the SamplerViewCso
// arm P4a adds, and a stub would prove only that the test can call itself.
TEST(DirectGLESSlotTable, ADeathNoticeForEveryP4aKindIsIdempotent) {
    using namespace MobileGL;
    using namespace MobileGL::MG_Backend::DirectGLES;
    using namespace MobileGL::MG_State::GLState;

    if (!EsprytSlotTablesEnabled()) {
        GTEST_SKIP() << "the legacy arm keys twins on the frontend heap address and cannot answer "
                        "a death notice at all";
    }

    const MG_State::GLState::StateObjectDeathOps* ops = MG_State::GLState::GetStateObjectDeathOps();
    ASSERT_NE(ops, nullptr) << "the backend installed no death-notice consumer";
    ASSERT_NE(ops->OnDestroyed, nullptr);

    // The sampler view first, because it is the arm P4a adds and the only kind whose notice
    // names a lifetime id belonging to ANOTHER object (the texture the view was minted off).
    {
        using SamplerViewImpl::BackendSamplerViewObject;
        using SamplerViewImpl::g_backendSamplerViews;

        auto owner = MakeShared<TextureObject2D>(0u);
        const Uint64 lifetimeId = owner->GetLifetimeId();
        const MG_Pipe::MGPipeHandle view =
            MG_Pipe::MGPipeSlots().Acquire(MG_Pipe::MGPipeKind::SamplerViewCso, lifetimeId);
        ASSERT_FALSE(MG_Pipe::MGPipeHandleIsNull(view));
        g_backendSamplerViews.GetOrCreate(view) = MakeShared<BackendSamplerViewObject>();
        ASSERT_NE(g_backendSamplerViews.FindByHandle(view), nullptr);

        ops->OnDestroyed(MG_Pipe::MGPipeKind::SamplerViewCso, lifetimeId);
        EXPECT_EQ(g_backendSamplerViews.FindByHandle(view), nullptr)
            << "the sampler-view notice left the twin behind, so the slot's next owner would "
               "inherit a dead texture's resolved view";
        EXPECT_TRUE(MG_Pipe::MGPipeHandleIsNull(
            MG_Pipe::MGPipeSlots().FindByLifetimeId(MG_Pipe::MGPipeKind::SamplerViewCso, lifetimeId)))
            << "the notice did not return the slot";

        // The second path. The client's helper calls Free right after raising the notice, and a
        // Free on a slot that is no longer live at that generation is a proven no-op
        // (SlotAllocator.cpp:117-119) - so this is the shape that actually ships, twice over.
        ops->OnDestroyed(MG_Pipe::MGPipeKind::SamplerViewCso, lifetimeId);
        MG_Pipe::MGPipeSlots().Free(MG_Pipe::MGPipeKind::SamplerViewCso, view);
        ops->OnDestroyed(MG_Pipe::MGPipeKind::SamplerViewCso, lifetimeId);
        EXPECT_EQ(g_backendSamplerViews.FindByHandle(view), nullptr);

        // And the slot really is back: a successor gets it with a moved generation, which is
        // the property a double free would break by skipping one.
        auto successor = MakeShared<TextureObject2D>(0u);
        const MG_Pipe::MGPipeHandle reused = MG_Pipe::MGPipeSlots().Acquire(
            MG_Pipe::MGPipeKind::SamplerViewCso, successor->GetLifetimeId());
        EXPECT_EQ(reused.Slot, view.Slot);
        EXPECT_NE(reused.Gen, view.Gen);
        MG_Pipe::MGPipeSlots().Free(MG_Pipe::MGPipeKind::SamplerViewCso, reused);
    }

    // The five kinds that DO have a frontend object: their notice is raised by the object's own
    // destructor today and by the client's helper after P4a's client packages land, so it is
    // delivered twice for one death. A second delivery must be a no-op rather than a second
    // free - which is what would drop a successor's twin under it.
    struct KindCase {
        const char* name;
        MG_Pipe::MGPipeKind kind;
        Uint64 lifetimeId;
        // m-9: the handle the object held before it died, so the ABA leg below can prove the
        // slot came back AT A MOVED GENERATION. ID-8's third case ("a notice for a slot
        // re-minted at a new generation") was covered for SamplerViewCso only; a double free
        // skips a generation, and nothing here would have caught that for the other five.
        MG_Pipe::MGPipeHandle handle;
    };
    Vector<KindCase> cases;
    {
        auto texture = MakeShared<TextureObject2D>(0u);
        auto renderbuffer = MakeShared<RenderbufferObject>(0u);
        auto framebuffer = MakeShared<FramebufferObject>(1u);
        auto sampler = MakeShared<SamplerObject>(0u);
        auto program = MakeShared<ProgramObject>(0u);

        TextureImpl::g_backendTextureObjects.GetOrCreate(SharedPtr<ITextureObject>(texture));
        RenderbufferImpl::g_backendRenderbufferObjects.GetOrCreate(renderbuffer);
        FramebufferImpl::g_backendFramebufferObjects.GetOrCreate(framebuffer);
        SamplerImpl::g_backendSamplerObjects.GetOrCreate(sampler);
        PrgramImpl::g_backendProgramObjects.GetOrCreate(program);

        const auto handleFor = [](MG_Pipe::MGPipeKind kind, Uint64 lifetimeId) {
            return MG_Pipe::MGPipeSlots().FindByLifetimeId(kind, lifetimeId);
        };
        cases.push_back({"Texture", MG_Pipe::MGPipeKind::Texture, texture->GetLifetimeId(),
                         handleFor(MG_Pipe::MGPipeKind::Texture, texture->GetLifetimeId())});
        cases.push_back({"Renderbuffer", MG_Pipe::MGPipeKind::Renderbuffer, renderbuffer->GetLifetimeId(),
                         handleFor(MG_Pipe::MGPipeKind::Renderbuffer, renderbuffer->GetLifetimeId())});
        cases.push_back({"Framebuffer", MG_Pipe::MGPipeKind::Framebuffer, framebuffer->GetLifetimeId(),
                         handleFor(MG_Pipe::MGPipeKind::Framebuffer, framebuffer->GetLifetimeId())});
        cases.push_back({"SamplerCso", MG_Pipe::MGPipeKind::SamplerCso, sampler->GetLifetimeId(),
                         handleFor(MG_Pipe::MGPipeKind::SamplerCso, sampler->GetLifetimeId())});
        cases.push_back({"ShaderCso", MG_Pipe::MGPipeKind::ShaderCso, program->GetLifetimeId(),
                         handleFor(MG_Pipe::MGPipeKind::ShaderCso, program->GetLifetimeId())});

        for (const auto& one : cases) {
            EXPECT_FALSE(MG_Pipe::MGPipeHandleIsNull(
                MG_Pipe::MGPipeSlots().FindByLifetimeId(one.kind, one.lifetimeId)))
                << one.name << ": nothing was twinned, so this walk proves nothing";
        }
        // The real destructors run here and raise the first notice.
    }

    for (const auto& one : cases) {
        EXPECT_TRUE(MG_Pipe::MGPipeHandleIsNull(
            MG_Pipe::MGPipeSlots().FindByLifetimeId(one.kind, one.lifetimeId)))
            << one.name << ": the object's own death did not return its slot";
        // Second and third deliveries: nothing to resolve, nothing to free, no abort.
        ops->OnDestroyed(one.kind, one.lifetimeId);
        ops->OnDestroyed(one.kind, one.lifetimeId);
        EXPECT_TRUE(MG_Pipe::MGPipeHandleIsNull(
            MG_Pipe::MGPipeSlots().FindByLifetimeId(one.kind, one.lifetimeId)))
            << one.name << ": a redundant notice resurrected a mapping";

        // m-9, THE ABA LEG: the slot really came back, and its generation MOVED. Two redundant
        // notices plus the object's own death are three chances to free one slot twice, and a
        // double free is invisible in every assertion above - it shows up here, as a successor
        // handed the same {slot, gen} the dead object held, which is precisely the handle a
        // surviving memo would still be naming.
        auto successor = MakeShared<TextureObject2D>(0u);
        const MG_Pipe::MGPipeHandle reused =
            MG_Pipe::MGPipeSlots().Acquire(one.kind, successor->GetLifetimeId());
        ASSERT_FALSE(MG_Pipe::MGPipeHandleIsNull(reused)) << one.name;
        if (reused.Slot == one.handle.Slot) {
            EXPECT_NE(reused.Gen, one.handle.Gen)
                << one.name << ": the slot came back at the SAME generation, so a memo holding "
                              "the dead object's handle would resolve to its successor's twin";
        }
        MG_Pipe::MGPipeSlots().Free(one.kind, reused);
    }
}

// The two-holder fix, end to end through the REAL Texture registry and the REAL destructor:
// a by-value copy of TextureImpl::g_backendTextureObjects - which is precisely what
// ScopedDirectGLESTextureBindings keeps in `previousRegistry` for the length of a test - must
// drop the twin on the same notice the registry global does, with no sweep and no second call.
TEST(DirectGLESSlotTable, ASavedCopyOfARealRegistryDropsTheTwinOnTheSameNotice) {
    using namespace MobileGL;
    using namespace MobileGL::MG_Backend::DirectGLES;
    using namespace MobileGL::MG_State::GLState;

    if (!EsprytSlotTablesEnabled()) {
        GTEST_SKIP() << "the legacy arm keys twins on the frontend heap address and cannot "
                        "answer a death notice";
    }

    auto& registry = TextureImpl::g_backendTextureObjects;
    SharedPtr<ITextureObject> texture = MakeShared<TextureObject2D>(0u);
    auto& twin = registry.GetOrCreate(texture);
    (void)twin;
    const MG_Pipe::MGPipeHandle handle = registry.HandleOf(texture.get());
    ASSERT_FALSE(MG_Pipe::MGPipeHandleIsNull(handle));

    // The fixture's shape: copy the registry while the twin is live.
    auto saved = registry;
    ASSERT_NE(saved.FindByHandle(handle), nullptr) << "the copy did not carry the live entry";

    // The real destructor raises the real notice; nothing else runs.
    texture.reset();
    EXPECT_EQ(registry.FindByHandle(handle), nullptr) << "the registry global kept the twin";
    EXPECT_EQ(saved.FindByHandle(handle), nullptr)
        << "the saved copy kept the dead texture's twin - the dispatcher told one holder only";
    EXPECT_FALSE(registry.DestroyByLifetimeId(0)) << "sanity: a null id frees nothing";
}

// The gate on MAJOR 1 of the round-3 review. Commit d89fb684 raised
// Fatal{PipeLegacyMemosDisabled} from inside InitDisplayAndContext(), i.e. from inside EGL
// bring-up - and the integration harness pre-flights EGL bring-up in a FORKED CHILD, converting
// any child that dies on a signal into "no usable GPU/display/ICD" and SKIPPING every scenario.
// So `MOBILEGL_PIPE_PUSH=0 MOBILEGL_PIPE_LEGACY_MEMOS=0 ctest -L integration-gpu -R DirectGLES`
// reported 100% tests passed while running nothing at all, on the exact pair of env vars the
// D14/D18 A/B is driven with. ROADMAP.md:7 forbids a gate that cannot go red for the reason it
// exists, and a lane that goes green by skipping is the worst version of that.
//
// The split this case pins: bring-up DIAGNOSES (and returns), first twin lookup STOPS. It
// checks the message and not only the signal, because an operator who is handed a bare
// "Subprocess aborted" has been told nothing about which two knobs they set.
TEST(DirectGLESSlotTable, AnArmlessKnobCombinationStopsInsteadOfSkippingTheLane) {
#if !MOBILEGL_PIPE_LEGACY_MEMOS
    GTEST_SKIP() << "this build compiles no legacy twin registry, so no knob combination can "
                    "leave the process without an arm";
#else
    using namespace MobileGL;
    namespace fs = std::filesystem;
    using MG_Backend::DirectGLES::EsprytSlotArmVerdict;

    // The pure half: all four knob combinations, no process required.
    EXPECT_EQ(MG_Backend::DirectGLES::ClassifyEsprytSlotArm(true, true), EsprytSlotArmVerdict::Handles);
    EXPECT_EQ(MG_Backend::DirectGLES::ClassifyEsprytSlotArm(true, false), EsprytSlotArmVerdict::Handles);
    EXPECT_EQ(MG_Backend::DirectGLES::ClassifyEsprytSlotArm(false, true), EsprytSlotArmVerdict::Legacy);
    EXPECT_EQ(MG_Backend::DirectGLES::ClassifyEsprytSlotArm(false, false), EsprytSlotArmVerdict::NoArm);

    // Both guards restore on every exit path - a failed ASSERT included - so no later case in
    // this binary runs on a mutated config or without the operator's file log, and the log
    // path is unique per process and per case (the round-4 review's minor 5).
    const ScopedArmlessKnobPair knobs;
    ASSERT_EQ(MG_Backend::DirectGLES::CurrentEsprytSlotArmVerdict(), EsprytSlotArmVerdict::NoArm);
    const ScopedLogFileRedirect log(UniqueScratchLogPath("mobilegl-espryt-armless-knobs"));

    // Bring-up's half of the split. It must NAME the knobs and it must RETURN: this call is the
    // one InitDisplayAndContext() makes, and it runs inside the harness's forked pre-flight
    // child. If it ever stops again, this line takes the whole binary down and the case is red.
    // (That the call SITE still makes this call and not the stopping one is
    // EglBringUpUnderTheArmlessKnobPairReturnsInsteadOfStopping below.)
    MG_Backend::DirectGLES::DiagnoseEsprytSlotArm();

#if !defined(_WIN32)
    // First-use's half: the stop, raised in a forked child so it is a datum rather than the end
    // of this process. In production the caller is a twin lookup inside a scenario body, where
    // ctest reports the crash as a FAILING test rather than as a missing GPU.
    EXPECT_EXIT((void)MG_Backend::DirectGLES::ResolveEsprytSlotTablesArm(),
                ::testing::KilledBySignal(SIGABRT), "");
#endif

    const std::string contents = log.Contents();
    ASSERT_FALSE(contents.empty()) << "neither the diagnosis nor the fatal wrote a line an "
                                      "operator could read";

    EXPECT_NE(contents.find("PipeLegacyMemosDisabled"), std::string::npos) << contents;
    EXPECT_NE(contents.find("MOBILEGL_PIPE_PUSH"), std::string::npos) << contents;
    EXPECT_NE(contents.find("MOBILEGL_PIPE_LEGACY_MEMOS=0"), std::string::npos) << contents;
    EXPECT_NE(contents.find("kMGPipeSubsystemEsprytSlots"), std::string::npos) << contents;
#if !defined(_WIN32)
    EXPECT_NE(contents.find("Fatal{"), std::string::npos)
        << "the diagnosis was logged but the first-use stop was not: " << contents;
#endif
#endif // MOBILEGL_PIPE_LEGACY_MEMOS
}

// The two guards the armless cases stand on, pinned on their own: whatever an operator had in
// MOBILEGL_LOG_FILE_PATH - a path, or nothing - and whatever MG_Config::Features held are back,
// byte for byte, once the guards go out of scope, with or without a failure inside. Before
// this the armless case unset the variable for every later case in the binary and restored
// the config only on its success path (the round-4 review's minor 5).
TEST(DirectGLESSlotTable, TheArmlessCasesLeaveTheLogPathAndTheConfigAsTheyFoundThem) {
    using namespace MobileGL;

    const Uint64 push = MG_Config::Features.PipePush;
    const Bool legacy = MG_Config::Features.PipeLegacyMemos;
    std::string previousPath;
    const bool hadPreviousPath = std::getenv("MOBILEGL_LOG_FILE_PATH") != nullptr;
    if (hadPreviousPath) previousPath = std::getenv("MOBILEGL_LOG_FILE_PATH");

    // With an operator path in place...
    const std::filesystem::path operatorPath = UniqueScratchLogPath("mobilegl-espryt-operator");
    SetEnvVar("MOBILEGL_LOG_FILE_PATH", operatorPath.string().c_str());
    {
        const ScopedArmlessKnobPair knobs;
        const ScopedLogFileRedirect redirect(UniqueScratchLogPath("mobilegl-espryt-guard"));
#if MOBILEGL_PIPE_LEGACY_MEMOS
        // Only a build with the legacy arm can be left armless; without it the verdict is
        // Handles whatever the knobs say, and what is pinned here is the restore, not the arm.
        EXPECT_EQ(MG_Backend::DirectGLES::CurrentEsprytSlotArmVerdict(),
                  MG_Backend::DirectGLES::EsprytSlotArmVerdict::NoArm);
#endif
        EXPECT_EQ(MG_Config::Features.PipePush & MG_Pipe::kMGPipeSubsystemEsprytSlots, 0ull);
        EXPECT_FALSE(MG_Config::Features.PipeLegacyMemos);
        EXPECT_STRNE(std::getenv("MOBILEGL_LOG_FILE_PATH"), operatorPath.string().c_str());
    }
    ASSERT_NE(std::getenv("MOBILEGL_LOG_FILE_PATH"), nullptr) << "the operator's log path was unset";
    EXPECT_STREQ(std::getenv("MOBILEGL_LOG_FILE_PATH"), operatorPath.string().c_str());
    EXPECT_EQ(MG_Config::Features.PipePush, push);
    EXPECT_EQ(MG_Config::Features.PipeLegacyMemos, legacy);

    // ...and with none.
    UnsetEnvVar("MOBILEGL_LOG_FILE_PATH");
    {
        const ScopedLogFileRedirect redirect(UniqueScratchLogPath("mobilegl-espryt-guard"));
        EXPECT_NE(std::getenv("MOBILEGL_LOG_FILE_PATH"), nullptr);
    }
    EXPECT_EQ(std::getenv("MOBILEGL_LOG_FILE_PATH"), nullptr)
        << "a log path was left behind where the operator had none";

    if (hadPreviousPath) {
        SetEnvVar("MOBILEGL_LOG_FILE_PATH", previousPath.c_str());
    }
    std::error_code ignored;
    std::filesystem::remove(operatorPath, ignored);
}

// The round-4 review's minor 4: the case above pins the two FUNCTIONS, and nothing failed if
// InitDisplayAndContext() (DirectGLES.cpp) was edited back to call the stopping one - which is
// exactly the regression that produced the round-3 major. This pins the CALL SITE, by running
// the real bring-up entry point under the armless pair.
//
// No display is needed: InitDisplayAndContext's twin-arm call is its first statement after
// the context teardown, ahead of eglGetDisplay, so an EGL table whose eglGetDisplay answers
// EGL_NO_DISPLAY takes bring-up through that call and straight back out with `false`. The
// child must then EXIT with the code below. If the site stops again it dies of SIGABRT
// instead, and the death test fails - naming both knobs - rather than skipping: in the
// integration harness that same abort is what turned into a green lane that ran nothing.
//
// Not skipped in a build without the legacy arm either: there the verdict is Handles and
// bring-up has nothing to diagnose, but it must still return, and this says so.
TEST(DirectGLESSlotTable, EglBringUpUnderTheArmlessKnobPairReturnsInsteadOfStopping) {
#if defined(_WIN32)
    GTEST_SKIP() << "needs a forked death test";
#else
    using namespace MobileGL;

    constexpr int kBringUpReturnedFalse = 0x51;
    constexpr int kBringUpReturnedTrue = 0x52;

    // The redirect is set up in the PARENT: the child inherits the environment and writes the
    // file, the parent reads it once the child has gone, and the guard restores the operator's
    // log path either way.
    const ScopedLogFileRedirect log(UniqueScratchLogPath("mobilegl-espryt-armless-bringup"));

    EXPECT_EXIT(
        {
            const ScopedArmlessKnobPair knobs;
            MG_External::EGLFunctionsTable egl{};
            egl.eglGetDisplay = +[](EGLNativeDisplayType) -> EGLDisplay { return EGL_NO_DISPLAY; };
            MG_Backend::DirectGLES::SetEGLFuncsTable(egl);
            const Bool ok = MG_Backend::DirectGLES::InitPbufferSurface(1, 1);
            MG_Util::Debug::Close();
            std::exit(ok ? kBringUpReturnedTrue : kBringUpReturnedFalse);
        },
        ::testing::ExitedWithCode(kBringUpReturnedFalse), "")
        << "EGL bring-up under MOBILEGL_PIPE_PUSH with kMGPipeSubsystemEsprytSlots clear and "
           "MOBILEGL_PIPE_LEGACY_MEMOS=0 did not RETURN: InitDisplayAndContext() is stopping "
           "on the armless knob pair again instead of diagnosing it, and the integration "
           "harness's forked pre-flight turns that stop into a lane that skips every scenario";

#if MOBILEGL_PIPE_LEGACY_MEMOS
    // And it diagnosed, by name, on the way through - the operator is told which two knobs
    // they set before the first draw - without a Fatal{} anywhere in bring-up.
    const std::string contents = log.Contents();
    EXPECT_NE(contents.find("PipeLegacyMemosDisabled"), std::string::npos)
        << "bring-up returned but did not diagnose the armless pair: " << contents;
    EXPECT_NE(contents.find("MOBILEGL_PIPE_PUSH"), std::string::npos) << contents;
    EXPECT_NE(contents.find("MOBILEGL_PIPE_LEGACY_MEMOS=0"), std::string::npos) << contents;
    EXPECT_EQ(contents.find("Fatal{"), std::string::npos)
        << "bring-up wrote a Fatal{} - the stop is back inside EGL bring-up: " << contents;
#endif
#endif
}

// P3a REWORK C-1's gate, and the case that would have caught it.
//
// A persistently mapped buffer whose store was NOT adopted - under the 16 MiB threshold, or
// with DisableLargeBufferAdoption, or with no EXT_buffer_storage - is written through its
// pointer and emits NOTHING: no resource call, no serial, no mutation epoch. The only thing
// that can make the next draw carry those bytes to the GPU is the per-draw clean probe
// answering DIRTY, which is why the pre-P3a probe asks the frontend IsMapped() instead of
// asking a serial. The first cut of the handle arm replaced that question with the applier
// record's HasLiveHostWrites - a field D-A4 pins false and nothing in P3a writes - so the
// answer became CLEAN forever, EnsureBufferResourceForHandle (and with it
// SyncPersistentMappedRange, the ONLY per-draw push for a vertex/uniform/SSBO persistent map)
// was never reached again, and the frame drew the last uploaded bytes with no diagnostic.
//
// The case is built so it can go red for exactly that: every other question is arranged to
// answer clean, and the first EXPECT asserts so before the map is taken.
TEST(DirectGLESBufferDrawProbe, ALiveHostMapKeepsTheHandleArmProbeDirtyBetweenTwoDraws) {
    using namespace MobileGL;
    using namespace MobileGL::MG_Backend::DirectGLES;
    using namespace MobileGL::MG_State::GLState;

    if (!EsprytSlotTablesEnabled()) {
        GTEST_SKIP() << "the handle-keyed resource table only exists on the {slot, gen} arm";
    }

    auto owner = MakeShared<BufferObject>(0u);
    owner->Respecify(256, nullptr);
    const MG_Pipe::MGPipeHandle res =
        MG_Pipe::MGPipeSlots().Acquire(MG_Pipe::MGPipeKind::Buffer, owner->GetLifetimeId());
    ASSERT_FALSE(MG_Pipe::MGPipeHandleIsNull(res));

    // The applier's record, written straight into the state rather than through package A's
    // entry points: this case is about the PROBE, and the probe reads the record.
    auto& applier = MG_Pipe::MGPipeApplier();
    if (applier.Resources.size() <= static_cast<SizeT>(res.Slot)) {
        applier.Resources.resize(static_cast<SizeT>(res.Slot) + 1);
    }
    auto& record = applier.Resources[res.Slot];
    record = {};
    record.Gen = res.Gen;
    record.Live = true;
    record.Desc.Width = 256;
    record.Serial = 7;
    // Pinned false by D-A4 and by the MOBILEGL_PIPE_VERIFY assertion in the probe. That is
    // exactly why it cannot answer the map question on its own.
    record.HasLiveHostWrites = false;

    auto& twin = BufferImpl::g_backendBufferResources.GetOrCreate(res);
    twin = MakeShared<BufferImpl::GLESBufferResource>();
    auto* const resource = twin.get();
    resource->id = 1; // a name, never used: this probe issues no GL
    resource->contextGeneration = BufferImpl::CurrentBufferContextGeneration();
    resource->storageInitialized = true;
    resource->storageSize = 256;
    resource->syncedChangeSerial = record.Serial;

    ASSERT_TRUE(BufferImpl::IsBufferDrawCleanByHandle(res, resource, owner.get()))
        << "the fixture is not clean before the map is taken, so this case cannot isolate the "
           "live-map question it exists for";

    // GL_MAP_PERSISTENT_BIT | GL_MAP_WRITE_BIT, shadow-backed (nothing adopts it: no backend
    // AcquirePersistentMap is registered in this binary). This is the state the app writes
    // through with no GL call at all.
    void* const mapped = owner->AcquireMemoryRange(
        Range1D{0, 256}, BufferMappingAccessBit::Write | BufferMappingAccessBit::Persistent);
    ASSERT_NE(mapped, nullptr);
    ASSERT_TRUE(owner->IsMapped());
    ASSERT_FALSE(owner->IsBackendPersistentMapped())
        << "the map was adopted, so this is the zero-copy case and not the one under test";

    // The write between the two draws. It moves NOTHING the record can see.
    static_cast<Uint8*>(mapped)[0] = 0x5Au;
    EXPECT_EQ(record.Serial, 7u) << "sanity: a write through a persistent map emits no call";

    EXPECT_FALSE(BufferImpl::IsBufferDrawCleanByHandle(res, resource, owner.get()))
        << "the handle arm called a persistently mapped, non-adopted buffer draw-CLEAN, so the "
           "bytes just written through its pointer would never reach the GPU: the live-map "
           "question has been answered from HasLiveHostWrites, which P3a pins false";

    owner->ReleaseMemory(false);
    EXPECT_TRUE(BufferImpl::IsBufferDrawCleanByHandle(res, resource, owner.get()))
        << "the probe stayed dirty after the unmap, i.e. it is not the map that is being read";

    BufferImpl::g_backendBufferResources.ReleaseByHandle(res);
    MG_Pipe::MGPipeSlots().Free(MG_Pipe::MGPipeKind::Buffer, res);
    record = {};
}

// P3a REWORK M-1's gate (contract-review M2). The minting overload's symmetric `!=` is safe
// because its handle comes out of the allocator and can never be behind the entry; the HANDLE
// overload's input ARRIVES in a payload, so a generation BEHIND the live entry's is reachable -
// and adopting it destroyed the incumbent's twin (a driver id, a persistent map, a pooled
// store, dropped by a defaulted destructor that deletes nothing) and stamped the slot back to
// the dead generation, after which the incumbent's own FindByHandle refused it.
TEST(DirectGLESSlotTable, AGenerationBehindTheLiveTwinIsRefusedRatherThanAdopted) {
    using namespace MobileGL;
    using namespace MobileGL::MG_Backend::DirectGLES;

    FakeSlotTable table;
    const MG_Pipe::MGPipeHandle current{7u, 2u};
    const MG_Pipe::MGPipeHandle stale{7u, 1u};

    auto& incumbent = table.GetOrCreate(current);
    incumbent = MakeShared<FakeBackendObject>();
    incumbent->marker = 0xC0FFEE;
    const FakeBackendObject* const raw = incumbent.get();

    auto& answer = table.GetOrCreate(stale);
    EXPECT_EQ(answer, nullptr)
        << "a backwards generation was handed a twin rather than refused; FindByHandle refuses "
           "the same input, so the two entry points disagreed";

    auto* const still = table.FindByHandle(current);
    ASSERT_NE(still, nullptr) << "the live twin's entry was retired by a handle from its past";
    EXPECT_EQ(still->get(), raw)
        << "the incumbent's twin was destroyed by a stale handle - the driver storage it owned "
           "went with it, and the incumbent would silently be handed a fresh empty twin";
    EXPECT_EQ((*still)->marker, 0xC0FFEE);

    // Forward is still a recycle, which is the direction the comment always covered.
    const MG_Pipe::MGPipeHandle successor{7u, 3u};
    auto& next = table.GetOrCreate(successor);
    EXPECT_EQ(next, nullptr) << "a successor at a recycled slot inherited its predecessor's twin";
    EXPECT_EQ(table.FindByHandle(current), nullptr) << "the predecessor's handle still resolves";

    // And a slot past the table's bound is refused rather than resized to.
    auto& absurd = table.GetOrCreate(MG_Pipe::MGPipeHandle{FakeSlotTable::kMaxHandleSlot, 1u});
    EXPECT_EQ(absurd, nullptr) << "an unbounded client slot decided a vector resize";
    EXPECT_EQ(table.FindByHandle(MG_Pipe::MGPipeHandle{FakeSlotTable::kMaxHandleSlot, 1u}), nullptr);
}

#else
// G2 wants the pull and the push build to list the SAME ctest entries. The twin table only
// exists under MOBILEGL_PIPE_PUSH, so in the pull build each case above keeps its name and
// skips visibly - a vanishing test is exactly what that gate is there to stop.
TEST(DirectGLESSlotTable, ARecycledSlotIsANewHandleAndTheStaleOneResolvesToNothing) {
    GTEST_SKIP() << "the {slot, gen} twin table is compiled only under MOBILEGL_PIPE_PUSH";
}

TEST(DirectGLESSlotTable, RepeatedLookupsOfALiveObjectKeepOneHandle) {
    GTEST_SKIP() << "the {slot, gen} twin table is compiled only under MOBILEGL_PIPE_PUSH";
}

TEST(DirectGLESSlotTable, FindNeverMutatesTheTable) {
    GTEST_SKIP() << "the {slot, gen} twin table is compiled only under MOBILEGL_PIPE_PUSH";
}

TEST(DirectGLESSlotTable, AWholeTableSavesResetsAndRestores) {
    GTEST_SKIP() << "the {slot, gen} twin table is compiled only under MOBILEGL_PIPE_PUSH";
}

TEST(DirectGLESSlotTable, TwoTablesOfTheSameKindShareOneSlotAndKeepTheirOwnTwin) {
    GTEST_SKIP() << "the {slot, gen} twin table is compiled only under MOBILEGL_PIPE_PUSH";
}

TEST(DirectGLESSlotTable, AnnouncedDeathKeepsObjectChurnFromAccumulatingWithoutASweep) {
    GTEST_SKIP() << "the {slot, gen} twin table is compiled only under MOBILEGL_PIPE_PUSH";
}

TEST(DirectGLESSlotTable, GetOrCreateToleratesANullStateObject) {
    GTEST_SKIP() << "the {slot, gen} twin table is compiled only under MOBILEGL_PIPE_PUSH";
}

TEST(DirectGLESSlotTable, TheTwinRegistryCasesInThisBinaryRunOnTheHandleArm) {
    GTEST_SKIP() << "the {slot, gen} twin table is compiled only under MOBILEGL_PIPE_PUSH";
}

TEST(DirectGLESSlotTable, AnAnnouncedDeathReturnsTheSlotWithoutASweep) {
    GTEST_SKIP() << "the {slot, gen} twin table is compiled only under MOBILEGL_PIPE_PUSH";
}

TEST(DirectGLESSlotTable, EveryReKeyedObjectClassAnnouncesItsOwnDeath) {
    GTEST_SKIP() << "the {slot, gen} twin table is compiled only under MOBILEGL_PIPE_PUSH";
}

TEST(DirectGLESSlotTable, TheHandleArmInstallsTheDeathNoticeConsumer) {
    GTEST_SKIP() << "the {slot, gen} twin table is compiled only under MOBILEGL_PIPE_PUSH";
}

TEST(DirectGLESSlotTable, AnArmlessKnobCombinationStopsInsteadOfSkippingTheLane) {
    GTEST_SKIP() << "the {slot, gen} twin table is compiled only under MOBILEGL_PIPE_PUSH";
}

TEST(DirectGLESSlotTable, EverySwitchedOverKindResolvesItsTwinThroughTheHandleArm) {
    GTEST_SKIP() << "the {slot, gen} twin table is compiled only under MOBILEGL_PIPE_PUSH";
}

TEST(DirectGLESSlotTable, ANegativeLookupIsNotCachedAcrossAnotherHoldersAcquire) {
    GTEST_SKIP() << "the {slot, gen} twin table is compiled only under MOBILEGL_PIPE_PUSH";
}

TEST(DirectGLESSlotTable, OneDeathNoticeDropsTheTwinInEveryHolderOfTheKind) {
    GTEST_SKIP() << "the {slot, gen} twin table is compiled only under MOBILEGL_PIPE_PUSH";
}

TEST(DirectGLESSlotTable, ASavedCopyOfARealRegistryDropsTheTwinOnTheSameNotice) {
    GTEST_SKIP() << "the {slot, gen} twin table is compiled only under MOBILEGL_PIPE_PUSH";
}

TEST(DirectGLESSlotTable, EglBringUpUnderTheArmlessKnobPairReturnsInsteadOfStopping) {
    GTEST_SKIP() << "the {slot, gen} twin table is compiled only under MOBILEGL_PIPE_PUSH";
}

TEST(DirectGLESSlotTable, TheArmlessCasesLeaveTheLogPathAndTheConfigAsTheyFoundThem) {
    GTEST_SKIP() << "the {slot, gen} twin table is compiled only under MOBILEGL_PIPE_PUSH";
}

TEST(DirectGLESSlotTable, AGenerationBehindTheLiveTwinIsRefusedRatherThanAdopted) {
    GTEST_SKIP() << "the {slot, gen} twin table is compiled only under MOBILEGL_PIPE_PUSH";
}

TEST(DirectGLESSlotTable, ADeathNoticeForEveryP4aKindIsIdempotent) {
    GTEST_SKIP() << "the {slot, gen} twin table is compiled only under MOBILEGL_PIPE_PUSH";
}

TEST(DirectGLESBufferDrawProbe, ALiveHostMapKeepsTheHandleArmProbeDirtyBetweenTwoDraws) {
    GTEST_SKIP() << "the handle-keyed resource table is compiled only under MOBILEGL_PIPE_PUSH";
}
#endif // MOBILEGL_PIPE_PUSH
