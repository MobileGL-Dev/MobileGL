#include <gtest/gtest.h>
#include <MG_State/EGLState/Core.h>
#include <MG_State/GLState/Core.h>
#include <EGL/eglext.h>
#include <future>
#include <memory>
#include <thread>
#include <vector>

namespace {
    using StateContext = MobileGL::MG_State::EGLState::EGLContext;

    struct EGLFixture {
        StateContext State;
        EGLDisplay Display = EGL_NO_DISPLAY;
        EGLConfig Config = nullptr;
        EGLSurface Surface = EGL_NO_SURFACE;
        StateContext::EGLContextHandle Context = EGL_NO_CONTEXT;
    };

    std::unique_ptr<EGLFixture> CreateFixture() {
        auto fixture = std::make_unique<EGLFixture>();
        fixture->Display = fixture->State.GetDisplay(EGL_DEFAULT_DISPLAY);
        EXPECT_NE(fixture->Display, EGL_NO_DISPLAY);
        EXPECT_TRUE(fixture->State.InitializeDisplay(fixture->Display, nullptr, nullptr));

        EGLint configCount = 0;
        EXPECT_TRUE(fixture->State.ChooseConfig(fixture->Display, nullptr, &fixture->Config, 1, &configCount));
        EXPECT_GE(configCount, 1);
        EXPECT_NE(fixture->Config, nullptr);

        const EGLint surfaceAttribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        fixture->Surface = fixture->State.CreatePbufferSurface(fixture->Display, fixture->Config, surfaceAttribs);
        EXPECT_NE(fixture->Surface, EGL_NO_SURFACE);

        fixture->Context = fixture->State.CreateContext(fixture->Display, fixture->Config, EGL_NO_CONTEXT, nullptr);
        EXPECT_NE(fixture->Context, EGL_NO_CONTEXT);
        return fixture;
    }
}

TEST(EGLStateMakeCurrent, ContextCannotBeCurrentOnTwoThreadsAtOnce) {
    auto fixture = CreateFixture();

    std::promise<void> threadAReady;
    std::promise<void> threadBAttempted;
    std::promise<void> threadAReleased;

    auto threadAReadyFuture = threadAReady.get_future();
    auto threadBAttemptedFuture = threadBAttempted.get_future();
    auto threadAReleasedFuture = threadAReleased.get_future();

    bool threadAAttachOk = false;
    bool threadAReleaseOk = false;
    bool threadBDenied = false;
    EGLint threadBDeniedError = EGL_SUCCESS;
    bool threadBAttachAfterReleaseOk = false;
    EGLint threadBAttachAfterReleaseError = EGL_SUCCESS;

    std::thread threadA([&] {
        threadAAttachOk = fixture->State.MakeCurrent(fixture->Display, fixture->Surface, fixture->Surface,
                                                      fixture->Context);
        threadAReady.set_value();
        threadBAttemptedFuture.wait();
        threadAReleaseOk =
            fixture->State.MakeCurrent(fixture->Display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        threadAReleased.set_value();
    });

    std::thread threadB([&] {
        threadAReadyFuture.wait();
        threadBDenied = fixture->State.MakeCurrent(fixture->Display, fixture->Surface, fixture->Surface,
                                                   fixture->Context);
        threadBDeniedError = fixture->State.ConsumeError();
        threadBAttempted.set_value();
        threadAReleasedFuture.wait();
        threadBAttachAfterReleaseOk =
            fixture->State.MakeCurrent(fixture->Display, fixture->Surface, fixture->Surface, fixture->Context);
        threadBAttachAfterReleaseError = fixture->State.ConsumeError();
        if (threadBAttachAfterReleaseOk) {
            (void)fixture->State.MakeCurrent(EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }
    });

    threadA.join();
    threadB.join();

    EXPECT_TRUE(threadAAttachOk);
    EXPECT_TRUE(threadAReleaseOk);
    EXPECT_FALSE(threadBDenied);
    EXPECT_EQ(threadBDeniedError, EGL_BAD_ACCESS);
    EXPECT_TRUE(threadBAttachAfterReleaseOk);
    EXPECT_EQ(threadBAttachAfterReleaseError, EGL_SUCCESS);
}

TEST(EGLStateMakeCurrent, SameThreadRepeatedAttachReleaseDoesNotLeaveStaleOwner) {
    auto fixture = CreateFixture();

    EXPECT_TRUE(fixture->State.MakeCurrent(fixture->Display, fixture->Surface, fixture->Surface, fixture->Context));
    EXPECT_TRUE(fixture->State.MakeCurrent(fixture->Display, fixture->Surface, fixture->Surface, fixture->Context));
    EXPECT_TRUE(fixture->State.MakeCurrent(fixture->Display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
    EXPECT_TRUE(fixture->State.MakeCurrent(fixture->Display, fixture->Surface, fixture->Surface, fixture->Context));
    EXPECT_TRUE(fixture->State.MakeCurrent(EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
    EXPECT_TRUE(fixture->State.MakeCurrent(fixture->Display, fixture->Surface, fixture->Surface, fixture->Context));
    EXPECT_TRUE(fixture->State.MakeCurrent(EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
    EXPECT_EQ(fixture->State.ConsumeError(), EGL_SUCCESS);
}

// The compatibility-profile accessor is affirmative-only (it backs GL_CONTEXT_PROFILE_MASK
// reporting): attrib-less contexts (profile mask 0) and released threads both answer "not
// compat" and therefore read as core-profile contexts.
TEST(EGLStateProfile, CompatibilityProfileRequiresExplicitCompatBit) {
    auto fixture = CreateFixture();

    EXPECT_FALSE(fixture->State.IsCurrentContextOpenGLCompatibilityProfile());

    EXPECT_TRUE(fixture->State.MakeCurrent(fixture->Display, fixture->Surface, fixture->Surface, fixture->Context));
    EXPECT_FALSE(fixture->State.IsCurrentContextOpenGLCompatibilityProfile());

    const EGLint compatAttribs[] = {EGL_CONTEXT_MAJOR_VERSION,
                                    3,
                                    EGL_CONTEXT_MINOR_VERSION,
                                    3,
                                    EGL_CONTEXT_OPENGL_PROFILE_MASK,
                                    EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT,
                                    EGL_NONE};
    const auto compatContext =
        fixture->State.CreateContext(fixture->Display, fixture->Config, EGL_NO_CONTEXT, compatAttribs);
    ASSERT_NE(compatContext, EGL_NO_CONTEXT);
    EXPECT_TRUE(fixture->State.MakeCurrent(fixture->Display, fixture->Surface, fixture->Surface, compatContext));
    EXPECT_TRUE(fixture->State.IsCurrentContextOpenGLCompatibilityProfile());
    EXPECT_FALSE(fixture->State.IsCurrentContextOpenGLCoreProfile());

    EXPECT_TRUE(fixture->State.MakeCurrent(EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
    EXPECT_FALSE(fixture->State.IsCurrentContextOpenGLCompatibilityProfile());
    EXPECT_EQ(fixture->State.ConsumeError(), EGL_SUCCESS);
}

// What `eglinfo -B` does for every API: eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)
// (EGL_KHR_surfaceless_context). It used to be EGL_BAD_MATCH, so eglinfo printed the EGL lines and
// no GL renderer/version lines at all. The binding records no surface: a client asking for the
// current surface must see what it bound.
TEST(EGLStateMakeCurrent, SurfacelessBindIsAcceptedAndRecordsNoSurface) {
    auto fixture = CreateFixture();
    EXPECT_TRUE(fixture->State.MakeCurrent(fixture->Display, EGL_NO_SURFACE, EGL_NO_SURFACE, fixture->Context));
    EXPECT_EQ(fixture->State.ConsumeError(), EGL_SUCCESS);
    EXPECT_EQ(fixture->State.GetCurrentContext(), fixture->Context);
    EXPECT_EQ(fixture->State.GetCurrentSurface(EGL_DRAW), EGL_NO_SURFACE);
    EXPECT_EQ(fixture->State.GetCurrentSurface(EGL_READ), EGL_NO_SURFACE);
    EXPECT_TRUE(fixture->State.MakeCurrent(EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
}

// EGL 1.5 3.7.2: a context current to a thread is destroyed once it is no longer current. The
// destroy answers EGL_TRUE (it used to be EGL_BAD_ACCESS), the handle is dead for any new use at
// once, the thread keeps it current until it releases it, and that release hands the context -
// with its GL state, whose last owner the caller now is - to whoever finishes the destroy.
TEST(EGLStateDestroyContext, DestroyingAContextCurrentOnAThreadIsDeferredUntilItsRelease) {
    auto fixture = CreateFixture();
    auto& state = fixture->State;
    const MobileGL::Uint64 token = state.GetContextClientToken(fixture->Context);
    std::weak_ptr<MobileGL::MG_State::GLState::GLContext> glState = state.GetContextGLState(fixture->Context);
    ASSERT_FALSE(glState.expired());

    std::promise<void> current;
    std::promise<void> destroyed;
    auto currentFuture = current.get_future();
    auto destroyedFuture = destroyed.get_future();
    bool stillCurrentAfterDestroy = false;
    bool rebindRefused = false;
    EGLint rebindError = EGL_SUCCESS;
    size_t reapedWhileCurrent = 0;
    std::thread owner([&] {
        EXPECT_TRUE(state.MakeCurrent(fixture->Display, fixture->Surface, fixture->Surface, fixture->Context));
        current.set_value();
        destroyedFuture.wait();
        stillCurrentAfterDestroy = state.GetCurrentContext() == fixture->Context;
        rebindRefused = !state.MakeCurrent(fixture->Display, fixture->Surface, fixture->Surface, fixture->Context);
        rebindError = state.ConsumeError();
        reapedWhileCurrent = state.TakeReapedContexts().size();
        EXPECT_TRUE(state.MakeCurrent(EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
    });
    currentFuture.wait();
    bool deferred = false;
    EXPECT_TRUE(state.DestroyContext(fixture->Display, fixture->Context, &deferred));
    EXPECT_TRUE(deferred);
    EXPECT_FALSE(state.ValidateContext(fixture->Context));
    EXPECT_FALSE(state.DestroyContext(fixture->Display, fixture->Context));
    EXPECT_EQ(state.ConsumeError(), EGL_BAD_CONTEXT);
    destroyed.set_value();
    owner.join();

    EXPECT_TRUE(stillCurrentAfterDestroy);
    EXPECT_TRUE(rebindRefused);
    EXPECT_EQ(rebindError, EGL_BAD_CONTEXT);
    EXPECT_EQ(reapedWhileCurrent, 0u);
    auto reaped = state.TakeReapedContexts();
    ASSERT_EQ(reaped.size(), 1u);
    EXPECT_EQ(reaped[0].Handle, fixture->Context);
    EXPECT_EQ(reaped[0].ClientContextToken, token);
    ASSERT_NE(reaped[0].GLStateObject, nullptr);
    // Nothing else holds the GL state - neither the book nor the thread that released it.
    reaped.clear();
    EXPECT_TRUE(glState.expired());
}

// A thread that had no GL state before its first bind does not keep the context's after releasing
// it. Keeping it made the thread's TLS the last owner of a context destroyed after the release, and
// the context's objects then died in the thread-exit destructor of that TLS, outside every entry
// point and its lock.
TEST(EGLStateDestroyContext, ReleasingThreadDoesNotKeepTheContextsGLState) {
    auto fixture = CreateFixture();
    auto& state = fixture->State;
    std::weak_ptr<MobileGL::MG_State::GLState::GLContext> glState = state.GetContextGLState(fixture->Context);
    std::thread worker([&] {
        EXPECT_TRUE(state.MakeCurrent(fixture->Display, fixture->Surface, fixture->Surface, fixture->Context));
        EXPECT_EQ(MobileGL::MG_State::pGLContext.get(), glState.lock().get());
        EXPECT_TRUE(state.MakeCurrent(EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
        EXPECT_NE(MobileGL::MG_State::pGLContext.get(), glState.lock().get());
        bool deferred = true;
        EXPECT_TRUE(state.DestroyContext(fixture->Display, fixture->Context, &deferred));
        EXPECT_FALSE(deferred);
        // Ended here, by the destroy - not later, when this thread exits.
        EXPECT_TRUE(glState.expired());
    });
    worker.join();
}

// A context created with its GL state deferred has none until BuildContextGLState builds it (a
// split client tells its server about the context first, then builds the state under it).
TEST(EGLStateCreateContext, DeferredGLStateIsBuiltOnRequestInTheShareGroup) {
    auto fixture = CreateFixture();
    auto& state = fixture->State;
    const auto shared = state.CreateContext(fixture->Display, fixture->Config, fixture->Context, nullptr,
                                            /*deferGLState=*/true);
    ASSERT_NE(shared, EGL_NO_CONTEXT);
    EXPECT_EQ(state.GetContextGLState(shared), nullptr);
    EXPECT_EQ(state.GetContextShareGroupToken(shared), state.GetContextShareGroupToken(fixture->Context));
    ASSERT_TRUE(state.BuildContextGLState(shared));
    const auto built = state.GetContextGLState(shared);
    ASSERT_NE(built, nullptr);
    EXPECT_EQ(built->GetShareGroup(), state.GetContextGLState(fixture->Context)->GetShareGroup());
    ASSERT_TRUE(state.BuildContextGLState(shared)); // idempotent
    EXPECT_EQ(state.GetContextGLState(shared), built);
}

// The extension relaxes only the both-absent case: one surface without the other stays a mismatch.
TEST(EGLStateMakeCurrent, OneSidedSurfaceIsStillAMismatch) {
    auto fixture = CreateFixture();
    EXPECT_FALSE(fixture->State.MakeCurrent(fixture->Display, fixture->Surface, EGL_NO_SURFACE, fixture->Context));
    EXPECT_EQ(fixture->State.ConsumeError(), EGL_BAD_MATCH);
    EXPECT_FALSE(fixture->State.MakeCurrent(fixture->Display, EGL_NO_SURFACE, fixture->Surface, fixture->Context));
    EXPECT_EQ(fixture->State.ConsumeError(), EGL_BAD_MATCH);
}

// eglinfo's ES probe chooses with EGL_RENDERABLE_TYPE/EGL_CONFORMANT = EGL_OPENGL_ES_BIT (ES 1.x),
// which no MobileGL config claims, so it passes the NULL config on to eglCreateContext - legal
// with EGL_KHR_no_config_context, where it means "no config". Without it that was
// EGL_BAD_CONFIG and the "OpenGL ES profile" lines never printed.
TEST(EGLStateCreateContext, NoConfigContextIsAcceptedAndReportsConfigIdZero) {
    auto fixture = CreateFixture();
    const EGLint esAttribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
    const auto context = fixture->State.CreateContext(fixture->Display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, esAttribs);
    ASSERT_NE(context, EGL_NO_CONTEXT);
    EXPECT_EQ(fixture->State.ConsumeError(), EGL_SUCCESS);
    EGLint configId = -1;
    EXPECT_TRUE(fixture->State.QueryContext(fixture->Display, context, EGL_CONFIG_ID, &configId));
    EXPECT_EQ(configId, 0);
    EXPECT_TRUE(fixture->State.MakeCurrent(fixture->Display, EGL_NO_SURFACE, EGL_NO_SURFACE, context));
    EXPECT_TRUE(fixture->State.MakeCurrent(EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
    // A non-null handle that names no config is still EGL_BAD_CONFIG.
    EXPECT_EQ(fixture->State.CreateContext(fixture->Display, reinterpret_cast<EGLConfig>(0xdead), EGL_NO_CONTEXT,
                                           esAttribs),
              EGL_NO_CONTEXT);
    EXPECT_EQ(fixture->State.ConsumeError(), EGL_BAD_CONFIG);
}

namespace {
    constexpr EGLAttrib kAbgr8888 = StateContext::kDrmFourccAbgr8888;
    constexpr EGLAttrib kXbgr8888 = StateContext::kDrmFourccXbgr8888;
    constexpr EGLAttrib kArgb8888 = StateContext::kDrmFourccArgb8888;
    constexpr EGLAttrib kXrgb8888 = StateContext::kDrmFourccXrgb8888;
    constexpr EGLAttrib kRgb565 = 0x36314752; // 'RG16': not a shared-image format

    // A complete single-plane import, as a compositor builds it for a linux-dmabuf wl_buffer.
    std::vector<EGLAttrib> DmaBufAttribs(EGLAttrib fourcc = kAbgr8888) {
        return {EGL_WIDTH, 64, EGL_HEIGHT, 32, EGL_LINUX_DRM_FOURCC_EXT, fourcc, EGL_DMA_BUF_PLANE0_FD_EXT, 7,
                EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0, EGL_DMA_BUF_PLANE0_PITCH_EXT, 256, EGL_NONE};
    }

    std::vector<EGLAttrib> WithAttrib(std::vector<EGLAttrib> attribs, EGLAttrib name, EGLAttrib value) {
        attribs.insert(attribs.end() - 1, {name, value});
        return attribs;
    }

    std::vector<EGLAttrib> WithoutAttrib(const std::vector<EGLAttrib>& attribs, EGLAttrib name) {
        std::vector<EGLAttrib> out;
        for (size_t i = 0; attribs[i] != EGL_NONE; i += 2) {
            if (attribs[i] != name) out.insert(out.end(), {attribs[i], attribs[i + 1]});
        }
        out.push_back(EGL_NONE);
        return out;
    }

    EGLint PrepareError(EGLFixture& fixture, const std::vector<EGLAttrib>& attribs,
                        StateContext::EGLContextHandle context = EGL_NO_CONTEXT, EGLClientBuffer buffer = nullptr) {
        StateContext::DmaBufImportAttribs parsed;
        const bool ok = fixture.State.PrepareDmaBufImport(fixture.Display, context, buffer, attribs.data(), &parsed);
        const EGLint error = fixture.State.ConsumeError();
        EXPECT_EQ(ok, error == EGL_SUCCESS);
        return error;
    }
} // namespace

TEST(EGLStateDmaBufImport, ParsesACompleteSinglePlaneImport) {
    auto fixture = CreateFixture();
    const auto attribs = WithAttrib(WithAttrib(DmaBufAttribs(kXbgr8888), EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
                                               static_cast<EGLAttrib>(0xffffffffu)),
                                    EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, 0x00ffffff);
    StateContext::DmaBufImportAttribs parsed;
    ASSERT_TRUE(fixture->State.PrepareDmaBufImport(fixture->Display, EGL_NO_CONTEXT, nullptr, attribs.data(), &parsed));
    EXPECT_EQ(fixture->State.ConsumeError(), EGL_SUCCESS);
    EXPECT_EQ(parsed.Width, 64);
    EXPECT_EQ(parsed.Height, 32);
    EXPECT_EQ(parsed.Fourcc, static_cast<uint32_t>(kXbgr8888));
    EXPECT_EQ(parsed.Fd, 7);
    EXPECT_EQ(parsed.Offset, 0);
    EXPECT_EQ(parsed.Pitch, 256);
    EXPECT_TRUE(parsed.HasModifier);
    EXPECT_EQ(parsed.Modifier, 0x00ffffffffffffffull);

    // The hints that only mean something for YUV are taken and ignored.
    EXPECT_EQ(PrepareError(*fixture, WithAttrib(DmaBufAttribs(), EGL_SAMPLE_RANGE_HINT_EXT, EGL_YUV_FULL_RANGE_EXT)),
              EGL_SUCCESS);
}

TEST(EGLStateDmaBufImport, RefusesWhatTheExtensionRefuses) {
    auto fixture = CreateFixture();
    // Incomplete lists.
    for (const EGLAttrib required : {EGL_WIDTH, EGL_HEIGHT, EGL_LINUX_DRM_FOURCC_EXT, EGL_DMA_BUF_PLANE0_FD_EXT,
                                     EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE0_PITCH_EXT}) {
        EXPECT_EQ(PrepareError(*fixture, WithoutAttrib(DmaBufAttribs(), required)), EGL_BAD_PARAMETER)
            << "missing 0x" << std::hex << required;
    }
    EXPECT_EQ(PrepareError(*fixture, WithAttrib(DmaBufAttribs(), EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, 0)),
              EGL_BAD_PARAMETER);
    // A context or a buffer, which this target takes neither of.
    EXPECT_EQ(PrepareError(*fixture, DmaBufAttribs(), fixture->Context), EGL_BAD_PARAMETER);
    int notABuffer = 0;
    EXPECT_EQ(PrepareError(*fixture, DmaBufAttribs(), EGL_NO_CONTEXT, &notABuffer), EGL_BAD_PARAMETER);
    // An attribute no dma-buf import has.
    EXPECT_EQ(PrepareError(*fixture, WithAttrib(DmaBufAttribs(), EGL_CONFIG_ID, 1)), EGL_BAD_PARAMETER);
    // A format no shared image has, and planes a single-plane format lacks.
    EXPECT_EQ(PrepareError(*fixture, DmaBufAttribs(kRgb565)), EGL_BAD_MATCH);
    EXPECT_EQ(PrepareError(*fixture, WithAttrib(DmaBufAttribs(), EGL_DMA_BUF_PLANE1_FD_EXT, 8)), EGL_BAD_ATTRIBUTE);
    // Values no image can have.
    auto zeroWidth = WithoutAttrib(DmaBufAttribs(), EGL_WIDTH);
    zeroWidth = WithAttrib(zeroWidth, EGL_WIDTH, 0);
    EXPECT_EQ(PrepareError(*fixture, zeroWidth), EGL_BAD_PARAMETER);
    auto badFd = WithAttrib(WithoutAttrib(DmaBufAttribs(), EGL_DMA_BUF_PLANE0_FD_EXT), EGL_DMA_BUF_PLANE0_FD_EXT, -1);
    EXPECT_EQ(PrepareError(*fixture, badFd), EGL_BAD_PARAMETER);
    auto badPitch =
        WithAttrib(WithoutAttrib(DmaBufAttribs(), EGL_DMA_BUF_PLANE0_PITCH_EXT), EGL_DMA_BUF_PLANE0_PITCH_EXT, 0);
    EXPECT_EQ(PrepareError(*fixture, badPitch), EGL_BAD_ACCESS);

    // An uninitialized display.
    ASSERT_TRUE(fixture->State.TerminateDisplay(fixture->Display));
    EXPECT_EQ(PrepareError(*fixture, DmaBufAttribs()), EGL_NOT_INITIALIZED);
}

TEST(EGLStateDmaBufImport, SharedImageLifetime) {
    auto fixture = CreateFixture();
    const StateContext::SharedImageInfo info{.Id = 42, .Width = 64, .Height = 32, .Fourcc = StateContext::kDrmFourccAbgr8888};
    const EGLImage image = fixture->State.CreateSharedImage(fixture->Display, info);
    ASSERT_NE(image, EGL_NO_IMAGE);

    StateContext::SharedImageInfo found;
    ASSERT_TRUE(fixture->State.GetSharedImage(image, &found));
    EXPECT_EQ(found.Id, 42u);
    EXPECT_EQ(found.Width, 64);
    EXPECT_EQ(found.Height, 32);

    // An image of another target names no shared image, and destroying it releases none.
    const EGLImage plain =
        fixture->State.CreateImage(fixture->Display, EGL_NO_CONTEXT, EGL_GL_TEXTURE_2D, nullptr, nullptr);
    ASSERT_NE(plain, EGL_NO_IMAGE);
    EXPECT_FALSE(fixture->State.GetSharedImage(plain, nullptr));
    uint64_t released = 99;
    EXPECT_TRUE(fixture->State.DestroyImage(fixture->Display, plain, &released));
    EXPECT_EQ(released, 0u);

    EXPECT_TRUE(fixture->State.DestroyImage(fixture->Display, image, &released));
    EXPECT_EQ(released, 42u);
    EXPECT_FALSE(fixture->State.GetSharedImage(image, nullptr));
    EXPECT_FALSE(fixture->State.DestroyImage(fixture->Display, image, &released));
    EXPECT_EQ(fixture->State.ConsumeError(), EGL_BAD_PARAMETER);

    // eglTerminate drops the display's images; their shared images are handed back once.
    ASSERT_NE(fixture->State.CreateSharedImage(fixture->Display, {.Id = 7, .Width = 1, .Height = 1}), EGL_NO_IMAGE);
    EXPECT_TRUE(fixture->State.TakeOrphanedSharedImages().empty());
    ASSERT_TRUE(fixture->State.TerminateDisplay(fixture->Display));
    EXPECT_EQ(fixture->State.TakeOrphanedSharedImages(), std::vector<uint64_t>{7});
    EXPECT_TRUE(fixture->State.TakeOrphanedSharedImages().empty());
}

TEST(EGLStateDmaBufImport, FormatAndModifierQueries) {
    auto fixture = CreateFixture();
    EGLint count = -1;
    ASSERT_TRUE(fixture->State.QueryDmaBufFormats(fixture->Display, true, 0, nullptr, &count));
    EXPECT_EQ(count, 4);
    EGLint formats[4] = {};
    ASSERT_TRUE(fixture->State.QueryDmaBufFormats(fixture->Display, true, 4, formats, &count));
    ASSERT_EQ(count, 4);
    EXPECT_EQ(formats[0], static_cast<EGLint>(kAbgr8888));
    EXPECT_EQ(formats[1], static_cast<EGLint>(kXbgr8888));
    EXPECT_EQ(formats[2], static_cast<EGLint>(kArgb8888));
    EXPECT_EQ(formats[3], static_cast<EGLint>(kXrgb8888));
    ASSERT_TRUE(fixture->State.QueryDmaBufFormats(fixture->Display, true, 1, formats, &count));
    EXPECT_EQ(count, 1);

    // Without shared images there is nothing to import.
    ASSERT_TRUE(fixture->State.QueryDmaBufFormats(fixture->Display, false, 0, nullptr, &count));
    EXPECT_EQ(count, 0);
    EXPECT_FALSE(fixture->State.QueryDmaBufFormats(fixture->Display, true, -1, formats, &count));
    EXPECT_EQ(fixture->State.ConsumeError(), EGL_BAD_PARAMETER);

    // No explicit modifiers: importers fall back to the implicit one.
    uint64_t modifiers[2] = {};
    EGLBoolean externalOnly[2] = {};
    count = -1;
    ASSERT_TRUE(fixture->State.QueryDmaBufModifiers(fixture->Display, true, static_cast<EGLint>(kAbgr8888), 2,
                                                    modifiers, externalOnly, &count));
    EXPECT_EQ(count, 0);
    EXPECT_FALSE(fixture->State.QueryDmaBufModifiers(fixture->Display, true, static_cast<EGLint>(kRgb565), 0,
                                                     nullptr, nullptr, &count));
    EXPECT_EQ(fixture->State.ConsumeError(), EGL_BAD_PARAMETER);

    ASSERT_TRUE(fixture->State.TerminateDisplay(fixture->Display));
    EXPECT_FALSE(fixture->State.QueryDmaBufFormats(fixture->Display, true, 0, nullptr, &count));
    EXPECT_EQ(fixture->State.ConsumeError(), EGL_NOT_INITIALIZED);
}

// EGL_EXT_create_context_robustness / EGL 1.5: the reset notification behavior a context is
// created with, under both names of the attribute (the extension's, which Chrome, KWin and Qt
// pass, and EGL 1.5's, which ANGLE's GL-on-EGL backend passes for its own native context), the
// errors the extension defines, and robust buffer access recorded as a request only.
TEST(EGLStateRobustness, ResetNotificationStrategyIsRecordedValidatedAndMatchedAcrossShares) {
    auto fixture = CreateFixture();
    auto& state = fixture->State;
    auto strategyOf = [&](StateContext::EGLContextHandle context) {
        EXPECT_TRUE(state.MakeCurrent(fixture->Display, fixture->Surface, fixture->Surface, context));
        const EGLint strategy = state.GetCurrentContextResetNotificationStrategy();
        EXPECT_TRUE(state.MakeCurrent(fixture->Display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
        return strategy;
    };

    // The default, and no context current at all.
    EXPECT_EQ(state.GetCurrentContextResetNotificationStrategy(), EGL_NO_RESET_NOTIFICATION);
    EXPECT_EQ(strategyOf(fixture->Context), EGL_NO_RESET_NOTIFICATION);

    const EGLint loseExt[] = {EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT, EGL_LOSE_CONTEXT_ON_RESET_EXT,
                              EGL_NONE};
    const auto robustExt = state.CreateContext(fixture->Display, fixture->Config, EGL_NO_CONTEXT, loseExt);
    ASSERT_NE(robustExt, EGL_NO_CONTEXT);
    EXPECT_EQ(strategyOf(robustExt), EGL_LOSE_CONTEXT_ON_RESET);

    // ANGLE's shape: an ES 3.2 request with the EGL 1.5 name.
    const EGLint loseCore[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2,
                               EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY, EGL_LOSE_CONTEXT_ON_RESET, EGL_NONE};
    const auto robustCore = state.CreateContext(fixture->Display, fixture->Config, EGL_NO_CONTEXT, loseCore);
    ASSERT_NE(robustCore, EGL_NO_CONTEXT);
    EXPECT_EQ(strategyOf(robustCore), EGL_LOSE_CONTEXT_ON_RESET);

    const EGLint noReset[] = {EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT, EGL_NO_RESET_NOTIFICATION_EXT,
                              EGL_NONE};
    const auto plain = state.CreateContext(fixture->Display, fixture->Config, EGL_NO_CONTEXT, noReset);
    ASSERT_NE(plain, EGL_NO_CONTEXT);
    EXPECT_EQ(strategyOf(plain), EGL_NO_RESET_NOTIFICATION);

    // A value that is neither: EGL_BAD_ATTRIBUTE, for both names.
    const EGLint badExt[] = {EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT, EGL_TRUE, EGL_NONE};
    EXPECT_EQ(state.CreateContext(fixture->Display, fixture->Config, EGL_NO_CONTEXT, badExt), EGL_NO_CONTEXT);
    EXPECT_EQ(state.ConsumeError(), EGL_BAD_ATTRIBUTE);
    const EGLint badCore[] = {EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY, 0, EGL_NONE};
    EXPECT_EQ(state.CreateContext(fixture->Display, fixture->Config, EGL_NO_CONTEXT, badCore), EGL_NO_CONTEXT);
    EXPECT_EQ(state.ConsumeError(), EGL_BAD_ATTRIBUTE);

    // Sharing: the two must agree (EGL_BAD_MATCH), in both directions; agreeing shares are made.
    EXPECT_EQ(state.CreateContext(fixture->Display, fixture->Config, robustExt, nullptr), EGL_NO_CONTEXT);
    EXPECT_EQ(state.ConsumeError(), EGL_BAD_MATCH);
    EXPECT_EQ(state.CreateContext(fixture->Display, fixture->Config, fixture->Context, loseExt), EGL_NO_CONTEXT);
    EXPECT_EQ(state.ConsumeError(), EGL_BAD_MATCH);
    const auto sharedRobust = state.CreateContext(fixture->Display, fixture->Config, robustExt, loseCore);
    ASSERT_NE(sharedRobust, EGL_NO_CONTEXT);
    EXPECT_EQ(state.GetContextShareGroupToken(sharedRobust), state.GetContextShareGroupToken(robustExt));
    EXPECT_EQ(strategyOf(sharedRobust), EGL_LOSE_CONTEXT_ON_RESET);
    const auto sharedPlain = state.CreateContext(fixture->Display, fixture->Config, fixture->Context, noReset);
    EXPECT_NE(sharedPlain, EGL_NO_CONTEXT);
}

TEST(EGLStateRobustness, RobustAccessIsARequestAndNeverAGLContextFlagByItself) {
    auto fixture = CreateFixture();
    auto& state = fixture->State;
    auto requestOf = [&](const EGLint* attribs, EGLint* flags) {
        const auto context = state.CreateContext(fixture->Display, fixture->Config, EGL_NO_CONTEXT, attribs);
        EXPECT_NE(context, EGL_NO_CONTEXT);
        EXPECT_TRUE(state.MakeCurrent(fixture->Display, fixture->Surface, fixture->Surface, context));
        const bool requested = state.IsCurrentContextRobustAccessRequested();
        *flags = state.GetCurrentContextFlags();
        EXPECT_TRUE(state.MakeCurrent(fixture->Display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
        return requested;
    };
    EGLint flags = 0;
    const EGLint none[] = {EGL_NONE};
    EXPECT_FALSE(requestOf(none, &flags));
    const EGLint ext[] = {EGL_CONTEXT_OPENGL_ROBUST_ACCESS_EXT, EGL_TRUE, EGL_NONE};
    EXPECT_TRUE(requestOf(ext, &flags));
    EXPECT_EQ(flags & GL_CONTEXT_FLAG_ROBUST_ACCESS_BIT, 0) << "GL_CONTEXT_FLAGS claims robust access";
    const EGLint core[] = {EGL_CONTEXT_OPENGL_ROBUST_ACCESS, EGL_TRUE, EGL_NONE};
    EXPECT_TRUE(requestOf(core, &flags));
    EXPECT_EQ(flags & GL_CONTEXT_FLAG_ROBUST_ACCESS_BIT, 0);
    // KWin's desktop candidate: the KHR flag bit, next to the debug bit that IS a GL flag.
    const EGLint khr[] = {EGL_CONTEXT_FLAGS_KHR, EGL_CONTEXT_OPENGL_ROBUST_ACCESS_BIT_KHR | EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR,
                          EGL_NONE};
    EXPECT_TRUE(requestOf(khr, &flags));
    EXPECT_EQ(flags & GL_CONTEXT_FLAG_ROBUST_ACCESS_BIT, 0);
    EXPECT_NE(flags & GL_CONTEXT_FLAG_DEBUG_BIT, 0);
    const EGLint off[] = {EGL_CONTEXT_OPENGL_ROBUST_ACCESS_EXT, EGL_FALSE, EGL_NONE};
    EXPECT_FALSE(requestOf(off, &flags));
}

#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

// EGL_ANDROID_native_fence_sync, the half that is pure state: a native fence sync owns a sync_file
// and answers its status, waits and copies from it. A pipe's read end stands in for the sync_file -
// it polls readable once something is written, as a fence does once it signals.
TEST(EGLStateNativeFence, AnImportedDescriptorDecidesStatusWaitsAndCopies) {
    auto fixture = CreateFixture();
    int fence[2] = {-1, -1};
    ASSERT_EQ(::pipe(fence), 0);
    const EGLAttrib attribs[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fence[0], EGL_NONE};
    const EGLSync sync = fixture->State.CreateSync(fixture->Display, EGL_SYNC_NATIVE_FENCE_ANDROID, attribs);
    ASSERT_NE(sync, EGL_NO_SYNC) << "error 0x" << std::hex << fixture->State.ConsumeError();

    EGLAttrib value = 0;
    ASSERT_TRUE(fixture->State.GetSyncAttrib(fixture->Display, sync, EGL_SYNC_TYPE, &value));
    EXPECT_EQ(value, static_cast<EGLAttrib>(EGL_SYNC_NATIVE_FENCE_ANDROID));
    ASSERT_TRUE(fixture->State.GetSyncAttrib(fixture->Display, sync, EGL_SYNC_CONDITION, &value));
    EXPECT_EQ(value, static_cast<EGLAttrib>(EGL_SYNC_NATIVE_FENCE_SIGNALED_ANDROID));
    ASSERT_TRUE(fixture->State.GetSyncAttrib(fixture->Display, sync, EGL_SYNC_STATUS, &value));
    EXPECT_EQ(value, static_cast<EGLAttrib>(EGL_UNSIGNALED));
    EXPECT_EQ(fixture->State.ClientWaitSync(fixture->Display, sync, 0, 0), EGL_TIMEOUT_EXPIRED);
    EXPECT_EQ(fixture->State.ClientWaitSync(fixture->Display, sync, 0, 5u * 1000 * 1000), EGL_TIMEOUT_EXPIRED);

    // A copy is a descriptor of the same fence: it signals with it.
    const EGLint copy = fixture->State.DupNativeFenceFD(fixture->Display, sync);
    ASSERT_GE(copy, 0);
    EXPECT_NE(copy, fence[0]);

    ASSERT_EQ(::write(fence[1], "s", 1), 1);
    ASSERT_TRUE(fixture->State.GetSyncAttrib(fixture->Display, sync, EGL_SYNC_STATUS, &value));
    EXPECT_EQ(value, static_cast<EGLAttrib>(EGL_SIGNALED));
    EXPECT_EQ(fixture->State.ClientWaitSync(fixture->Display, sync, 0, EGL_FOREVER), EGL_CONDITION_SATISFIED);
    EXPECT_TRUE(fixture->State.WaitSync(fixture->Display, sync, 0));
    pollfd entry{copy, POLLIN, 0};
    EXPECT_EQ(::poll(&entry, 1, 0), 1);

    // The sync owned its descriptor: destroying it closes that, and the copy stays the caller's.
    EXPECT_TRUE(fixture->State.DestroySync(fixture->Display, sync));
    EXPECT_EQ(::fcntl(fence[0], F_GETFD), -1);
    EXPECT_NE(::fcntl(copy, F_GETFD), -1);
    ::close(copy);
    ::close(fence[1]);
}

// A fence command whose work completed with no descriptor to show for it is signaled, and has no
// descriptor to copy; a native fence without one is no import either, and an ordinary fence has
// none to copy.
TEST(EGLStateNativeFence, AFenceCommandWithoutADescriptorIsSignaledAndHasNoneToCopy) {
    auto fixture = CreateFixture();
    const EGLSync done = fixture->State.CreateNativeFenceSync(fixture->Display, -1, EGL_SYNC_PRIOR_COMMANDS_COMPLETE);
    ASSERT_NE(done, EGL_NO_SYNC);
    EGLAttrib value = 0;
    ASSERT_TRUE(fixture->State.GetSyncAttrib(fixture->Display, done, EGL_SYNC_STATUS, &value));
    EXPECT_EQ(value, static_cast<EGLAttrib>(EGL_SIGNALED));
    ASSERT_TRUE(fixture->State.GetSyncAttrib(fixture->Display, done, EGL_SYNC_CONDITION, &value));
    EXPECT_EQ(value, static_cast<EGLAttrib>(EGL_SYNC_PRIOR_COMMANDS_COMPLETE));
    EXPECT_EQ(fixture->State.ClientWaitSync(fixture->Display, done, 0, 0), EGL_CONDITION_SATISFIED);
    EXPECT_EQ(fixture->State.DupNativeFenceFD(fixture->Display, done), EGL_NO_NATIVE_FENCE_FD_ANDROID);
    EXPECT_EQ(fixture->State.ConsumeError(), EGL_BAD_PARAMETER);
    EXPECT_TRUE(fixture->State.DestroySync(fixture->Display, done));

    const EGLAttrib none[] = {EGL_NONE};
    EXPECT_EQ(fixture->State.CreateSync(fixture->Display, EGL_SYNC_NATIVE_FENCE_ANDROID, none), EGL_NO_SYNC);
    EXPECT_EQ(fixture->State.ConsumeError(), EGL_BAD_ATTRIBUTE);

    const EGLSync plain = fixture->State.CreateSync(fixture->Display, EGL_SYNC_FENCE, none);
    ASSERT_NE(plain, EGL_NO_SYNC);
    EXPECT_EQ(fixture->State.DupNativeFenceFD(fixture->Display, plain), EGL_NO_NATIVE_FENCE_FD_ANDROID);
    EXPECT_EQ(fixture->State.ConsumeError(), EGL_BAD_PARAMETER);
    EXPECT_TRUE(fixture->State.DestroySync(fixture->Display, plain));
}
#endif
