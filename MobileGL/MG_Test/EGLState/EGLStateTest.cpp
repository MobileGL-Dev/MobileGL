#include <gtest/gtest.h>
#include <MG_State/EGLState/Core.h>
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
