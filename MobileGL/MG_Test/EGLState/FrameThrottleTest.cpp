#include <gtest/gtest.h>
#include <MG_Impl/EGLImpl/FrameThrottle.h>
#include <MG_State/EGLState/Core.h>

// A Wayland window's swaps are paced by the compositor's frame callbacks when the swap interval is
// above 0 - which is what lets every client idle while the compositor shows nothing - and never
// when it is 0.

using MobileGL::MG_Impl::EGLImpl::Wayland::FrameThrottle;

TEST(WaylandFrameThrottle, IntervalOneWaitsForTheCallbackOfThePreviousCommit) {
    FrameThrottle throttle;
    EXPECT_EQ(throttle.WaitBudgetMs(1, 0), 0); // the first swap has nothing to wait for
    ASSERT_TRUE(throttle.WantsFrameRequest());
    throttle.Requested(0);
    EXPECT_EQ(throttle.WaitBudgetMs(1, 1), FrameThrottle::kForever); // the next swap waits for it ...
    EXPECT_FALSE(throttle.WantsFrameRequest());                      // ... and asks for no second one
    throttle.Done();
    EXPECT_EQ(throttle.WaitBudgetMs(1, 2), 0); // the compositor used the frame
    EXPECT_TRUE(throttle.WantsFrameRequest());
}

TEST(WaylandFrameThrottle, IntervalZeroStaysUnpacedWhileTheCompositorAnswers) {
    FrameThrottle throttle;
    throttle.Requested(1000);
    EXPECT_EQ(throttle.WaitBudgetMs(0, 1000), 0);
    EXPECT_EQ(throttle.WaitBudgetMs(0, 1000 + FrameThrottle::kStarvedAfterMs - 1), 0);
    throttle.Done();
    throttle.Requested(1016);
    EXPECT_EQ(throttle.WaitBudgetMs(0, 1030), 0);
}

TEST(WaylandFrameThrottle, IntervalZeroSlowsToTheStarvedPaceWhenNothingIsShown) {
    FrameThrottle throttle;
    throttle.Requested(5000);
    // The compositor shows nothing: the callback stays unanswered, every later swap waits a while.
    EXPECT_EQ(throttle.WaitBudgetMs(0, 5000 + FrameThrottle::kStarvedAfterMs), FrameThrottle::kStarvedWaitMs);
    EXPECT_EQ(throttle.WaitBudgetMs(0, 60000), FrameThrottle::kStarvedWaitMs);
    // Shown again: the first answer ends it.
    throttle.Done();
    EXPECT_EQ(throttle.WaitBudgetMs(0, 60001), 0);
}

TEST(EGLStateSwapInterval, DefaultsToOneAndFollowsEglSwapInterval) {
    MobileGL::MG_State::EGLState::EGLContext state;
    const EGLDisplay display = state.GetDisplay(EGL_DEFAULT_DISPLAY);
    ASSERT_NE(display, EGL_NO_DISPLAY);
    ASSERT_TRUE(state.InitializeDisplay(display, nullptr, nullptr));
    EXPECT_EQ(state.GetSwapInterval(display), 1);
    ASSERT_TRUE(state.SwapInterval(display, 0));
    EXPECT_EQ(state.GetSwapInterval(display), 0);
    EXPECT_FALSE(state.SwapInterval(display, 99)); // out of the config's range: refused, unchanged
    EXPECT_EQ(state.GetSwapInterval(display), 0);
    EXPECT_EQ(state.GetSwapInterval(EGL_NO_DISPLAY), 1);
}
