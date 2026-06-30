#include <gtest/gtest.h>
#include <MG_State/EGLState/Core.h>
#include <future>
#include <memory>
#include <thread>

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
