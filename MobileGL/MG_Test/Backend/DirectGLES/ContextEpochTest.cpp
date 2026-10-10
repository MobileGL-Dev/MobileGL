// MobileGL - P5f fs production-state epoch regression tests.
#include <gtest/gtest.h>
#include <Config.h>
#include <MG_Backend/DirectGLES/DirectGLES.h>
#include <MG_Backend/DirectGLES/Managers.h>
#if MOBILEGL_BUILD_DISAGGREGATED
#include <MG_Backend/MGPipe/PipeInputs.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_State/GLState/Core.h>
#include <MG_Remote/Client/ClientSession.h>
#include <thread>
#include <atomic>

namespace MobileGL::MG_Backend::DirectGLES {
    SamplerImpl::BackendSamplerObject* GetRawDepthFetchSampler();
}
#endif

using namespace MobileGL;
using namespace MobileGL::MG_Backend::DirectGLES;

namespace {
#if MOBILEGL_BUILD_DISAGGREGATED
    Uint unpackCalls = 0, samplerParameters = 0, nextNativeId = 100, boundXfb = 0;
    Vector<GLenum> unpackNames;
    void GL_APIENTRY PixelStore(GLenum name, GLint) { ++unpackCalls; unpackNames.push_back(name); }
    void GL_APIENTRY GenNames(GLsizei count, GLuint* names) { while (count-- > 0) *names++ = ++nextNativeId; }
    void GL_APIENTRY DeleteNames(GLsizei, const GLuint*) {}
    void GL_APIENTRY SamplerParam(GLuint, GLenum, GLint) { ++samplerParameters; }
    void GL_APIENTRY BindXfb(GLenum, GLuint id) { boundXfb = id; }
    void GL_APIENTRY NoArgs() {}
    void GL_APIENTRY BeginXfb(GLenum) {}
    void GL_APIENTRY XfbVaryings(GLuint, GLsizei, const GLchar* const*, GLenum) {}
    struct DriverScope {
        MG_External::GLESFunctionsTable Functions = g_GLESFuncs;
        MG_Config::TransportMode Transport = MG_Config::Transport;
        DriverScope() {
            ++g_backendContextGeneration;
            MG_Config::Transport = MG_Config::TransportMode::InProcess;
            g_GLESFuncs.glPixelStorei = &PixelStore;
            g_GLESFuncs.glGenSamplers = &GenNames;
            g_GLESFuncs.glDeleteSamplers = &DeleteNames;
            g_GLESFuncs.glSamplerParameteri = &SamplerParam;
            g_GLESFuncs.glGenTransformFeedbacks = &GenNames;
            g_GLESFuncs.glDeleteTransformFeedbacks = &DeleteNames;
            g_GLESFuncs.glBindTransformFeedback = &BindXfb;
            g_GLESFuncs.glPauseTransformFeedback = &NoArgs;
            g_GLESFuncs.glResumeTransformFeedback = &NoArgs;
            g_GLESFuncs.glBeginTransformFeedback = &BeginXfb;
            g_GLESFuncs.glEndTransformFeedback = &NoArgs;
            g_GLESFuncs.glTransformFeedbackVaryings = &XfbVaryings;
        }
        ~DriverScope() {
            XfbImpl::OnBackendContextDestroyed();
            ++g_backendContextGeneration;
            g_GLESFuncs = Functions;
            MG_Config::Transport = Transport;
        }
    };
#endif
}

TEST(ContextEpochTest, UnpackDefaultIsRepublishedForANewNativeContextOrRole) {
#if MOBILEGL_BUILD_DISAGGREGATED
    DriverScope scope;
    unpackCalls = 0;
    TextureImpl::ExerciseDefaultUnpackScopeForTesting();
    EXPECT_EQ(unpackCalls, 8u); // six defaults, tight alignment, restore
    unpackCalls = 0;
    TextureImpl::ExerciseDefaultUnpackScopeForTesting();
    EXPECT_EQ(unpackCalls, 2u);
    ++g_backendContextGeneration;
    unpackCalls = 0;
    TextureImpl::ExerciseDefaultUnpackScopeForTesting();
    EXPECT_EQ(unpackCalls, 8u) << "a previous context's unpack shadow suppressed the new context's default writes";
    MG_Config::Transport = MG_Config::TransportMode::Monolith;
    unpackCalls = 0;
    TextureImpl::ExerciseDefaultUnpackScopeForTesting();
    EXPECT_EQ(unpackCalls, 8u) << "the other role inherited the server's unpack shadow";
#else
    GTEST_SKIP() << "context-role isolation is a disaggregated-build mechanism";
#endif
}

// THE TRANSPORT-NATIVE ARM, which is the one a served context takes. DriverScope pins
// TransportMode::InProcess, so GetRawDepthFetchSampler() returns early at DirectGLES.cpp:343 and
// never reaches the monolith pair below it.
TEST(ContextEpochTest, RawDepthSamplerOwnsANativeIdForEachContextGeneration) {
#if MOBILEGL_BUILD_DISAGGREGATED
    DriverScope scope;
    samplerParameters = 0;
    const Uint first = GetRawDepthFetchSampler()->GetBackendSamplerId();
    EXPECT_EQ(samplerParameters, 4u);
    EXPECT_EQ(GetRawDepthFetchSampler()->GetBackendSamplerId(), first);
    EXPECT_EQ(samplerParameters, 4u);
    ++g_backendContextGeneration;
    const Uint second = GetRawDepthFetchSampler()->GetBackendSamplerId();
    EXPECT_NE(first, second) << "a sampler id outlived its native context";
    EXPECT_EQ(samplerParameters, 8u);
#else
    GTEST_SKIP() << "native server sampler exists only in the disaggregated build";
#endif
}


TEST(ContextEpochTest, SameXfbNameInTwoServedContextsDoesNotAliasThePausedSpan) {
#if MOBILEGL_BUILD_DISAGGREGATED
    DriverScope scope;
    auto& state = MG_Pipe::MGPipeApplier();
    const auto saved = state.BoundStreamOutputLifetimeId;
    auto firstContext = MakeUnique<MG_State::GLState::GLContext>();
    auto secondContext = MakeUnique<MG_State::GLState::GLContext>();
    const Uint64 firstLifetime = firstContext->GetBoundTransformFeedbackLifetimeId();
    const Uint64 secondLifetime = secondContext->GetBoundTransformFeedbackLifetimeId();
    ASSERT_NE(firstLifetime, 0u);
    ASSERT_NE(secondLifetime, 0u);
    ASSERT_NE(firstLifetime, secondLifetime);
    state.BoundStreamOutputLifetimeId = firstLifetime;
    XfbImpl::BindTransformFeedback(0);
    const Uint first = boundXfb;
    EXPECT_NE(first, 0u) << "virtual name 0 needs its own native object";
    XfbImpl::BeginTransformFeedback(GL_POINTS);
    EXPECT_TRUE(XfbImpl::IsCaptureSpanOpen());
    XfbImpl::PauseTransformFeedback();
    EXPECT_FALSE(XfbImpl::IsCaptureSpanOpen());
    state.BoundStreamOutputLifetimeId = secondLifetime;
    XfbImpl::BindTransformFeedback(0);
    EXPECT_NE(boundXfb, 0u);
    EXPECT_NE(first, boundXfb) << "the GL name was mistaken for cross-context identity";
    EXPECT_FALSE(XfbImpl::IsCaptureSpanOpen());
    XfbImpl::BeginTransformFeedback(GL_POINTS);
    EXPECT_TRUE(XfbImpl::IsCaptureSpanOpen());
    state.BoundStreamOutputLifetimeId = firstLifetime;
    EXPECT_FALSE(XfbImpl::IsCaptureSpanOpen()) << "returning to the paused object inherited another context's span";
    EXPECT_EQ(boundXfb, first);
    XfbImpl::ResumeTransformFeedback();
    EXPECT_TRUE(XfbImpl::IsCaptureSpanOpen());
    ++g_backendContextGeneration;
    EXPECT_FALSE(XfbImpl::IsCaptureSpanOpen()) << "native context replacement retained a dead capture";
    EXPECT_NE(boundXfb, first);
    state.BoundStreamOutputLifetimeId = saved;
#else
    GTEST_SKIP() << "server XFB identity exists only in the disaggregated build";
#endif
}

TEST(ContextEpochTest, ServerLivenessDoesNotFollowTheClientContextOrAVerbStamp) {
#if MOBILEGL_BUILD_DISAGGREGATED
    DriverScope scope;
    auto context = MG_State::pGLContext.Take();
    const Bool previous = MG_Pipe::MGPipeServerContextIsLive();
    MG_Pipe::MGPipeServerSetContextLive(false);
    MG_State::pGLContext = MakeUnique<MG_State::GLState::GLContext>();
    EXPECT_FALSE(MG_Pipe::gPipeInputs->IsLive());
    MG_Pipe::MGPipeServerSetContextLive(true);
    MG_State::pGLContext.reset();
    EXPECT_TRUE(MG_Pipe::gPipeInputs->IsLive());
    MG_Pipe::MGPipeServerSetContextLive(false);
    MG_Pipe::MGPipeServerStampVerbBoundary(MG_Pipe::MGPipeVerb::DrawArrays);
    EXPECT_FALSE(MG_Pipe::gPipeInputs->IsLive()) << "a stale command resurrected a destroyed server context";
    MG_Pipe::MGPipeServerClearVerbBoundary();
    MG_State::pGLContext = Move(context);
    MG_Pipe::MGPipeServerSetContextLive(previous);
#else
    GTEST_SKIP() << "server liveness exists only in the disaggregated build";
#endif
}

TEST(ContextEpochTest, DualBlockApplierDiagnosticIsRoleLocalWhileSharedControlRemainsArmed) {
#if MOBILEGL_BUILD_DISAGGREGATED
    DriverScope scope;
    const Bool previous = MG_Config::Ipc.RoleSplitState;
    using Session = MG_Remote::Client::ClientSession;
    const auto observe = [&](Bool split) {
        MG_Config::Ipc.RoleSplitState = split;
        std::atomic<Bool> entered{false}, leave{false};
        Bool serverSawItself = false;
        std::thread server([&] {
            Session::NoteApplyThreadEnteredApplier();
            serverSawItself = Session::ApplyThreadIsInsideApplier();
            entered.store(true, std::memory_order_release);
            while (!leave.load(std::memory_order_acquire)) std::this_thread::yield();
            Session::NoteApplyThreadLeftApplier();
        });
        while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();
        const Bool clientSawServer = Session::ApplyThreadIsInsideApplier();
        leave.store(true, std::memory_order_release);
        server.join();
        EXPECT_TRUE(serverSawItself);
        EXPECT_EQ(clientSawServer, !split);
        EXPECT_FALSE(Session::ApplyThreadIsInsideApplier());
    };
    observe(false);
    observe(true);
    MG_Config::Ipc.RoleSplitState = previous;
#else
    GTEST_SKIP() << "role-local observer requires the disaggregated build";
#endif
}

// THE SCISSOR GUARD AROUND A COPY THE BACKEND MAKES ON ITS OWN (the shared-image present, the
// readbacks). The render-state shadow is one per process and is only synced at draws, so after a
// context switch or another session's turn its bits are another context's. A client that presents
// without drawing since then (a frame that redraws nothing) kept its own scissor on through the
// copy - the image was clipped to the client's last box and kept the frame it held before.
#if MOBILEGL_BUILD_DISAGGREGATED
namespace {
    Bool fakeScissor = false;
    Uint isEnabledCalls = 0;
    void GL_APIENTRY FakeEnable(GLenum cap) { if (cap == GL_SCISSOR_TEST) fakeScissor = true; }
    void GL_APIENTRY FakeDisable(GLenum cap) { if (cap == GL_SCISSOR_TEST) fakeScissor = false; }
    GLboolean GL_APIENTRY FakeIsEnabled(GLenum cap) {
        ++isEnabledCalls;
        return cap == GL_SCISSOR_TEST && fakeScissor ? GL_TRUE : GL_FALSE;
    }
    struct ScissorDriver : DriverScope {
        ScissorDriver() {
            g_GLESFuncs.glEnable = &FakeEnable;
            g_GLESFuncs.glDisable = &FakeDisable;
            g_GLESFuncs.glIsEnabled = &FakeIsEnabled;
            isEnabledCalls = 0;
        }
        ~ScissorDriver() { RenderStateImpl::InvalidateSyncedRenderState(); }
    };
    // The driver's scissor as the copy inside the guard sees it.
    Bool ScissorDuringGuard() {
        Bool seen = true;
        RenderStateImpl::RunScissorDisabledForTesting([](void* out) { *static_cast<Bool*>(out) = fakeScissor; },
                                                      &seen);
        return seen;
    }
} // namespace
#endif

TEST(ContextEpochTest, ScissorGuardTurnsOffTheScissorTheShadowNoLongerDescribes) {
#if MOBILEGL_BUILD_DISAGGREGATED
    ScissorDriver scope;
    // Another context last synced the shadow with the test off; then the switch to this one.
    RenderStateImpl::SetSyncedScissorTestForTesting(false);
    RenderStateImpl::InvalidateSyncedRenderState();
    fakeScissor = true; // this context's application left its scissor on
    EXPECT_FALSE(ScissorDuringGuard()) << "the copy ran clipped by the application's scissor box";
    EXPECT_TRUE(fakeScissor) << "the guard did not put the application's scissor back";

    // The same through a context epoch moving without an explicit invalidation.
    RenderStateImpl::SetSyncedScissorTestForTesting(false);
    ++g_backendContextGeneration;
    fakeScissor = true;
    EXPECT_FALSE(ScissorDuringGuard()) << "a shadow synced in another context epoch was trusted";
    EXPECT_TRUE(fakeScissor);
#else
    GTEST_SKIP() << "the per-session render-state shadow hazard is a multi-session server's";
#endif
}

TEST(ContextEpochTest, ScissorGuardLeavesAnOffScissorOffWhateverAStaleShadowSays) {
#if MOBILEGL_BUILD_DISAGGREGATED
    ScissorDriver scope;
    RenderStateImpl::SetSyncedScissorTestForTesting(true);
    RenderStateImpl::InvalidateSyncedRenderState();
    fakeScissor = false;
    EXPECT_FALSE(ScissorDuringGuard());
    EXPECT_FALSE(fakeScissor) << "the guard turned on a scissor this context never had";
#else
    GTEST_SKIP() << "the per-session render-state shadow hazard is a multi-session server's";
#endif
}

TEST(ContextEpochTest, ScissorGuardTakesACurrentShadowsWordWithoutAskingTheDriver) {
#if MOBILEGL_BUILD_DISAGGREGATED
    ScissorDriver scope;
    RenderStateImpl::SetSyncedScissorTestForTesting(true);
    fakeScissor = true;
    EXPECT_FALSE(ScissorDuringGuard());
    EXPECT_TRUE(fakeScissor);
    EXPECT_EQ(isEnabledCalls, 0u) << "a shadow synced in this context should answer without a driver query";
#else
    GTEST_SKIP() << "the per-session render-state shadow hazard is a multi-session server's";
#endif
}
