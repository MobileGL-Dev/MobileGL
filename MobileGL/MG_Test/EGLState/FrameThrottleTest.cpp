#include <gtest/gtest.h>
#include <MG_Impl/EGLImpl/FrameThrottle.h>
#include <MG_State/EGLState/Core.h>

// A Wayland window's swaps are paced by the compositor's frame callbacks when the swap interval is
// above 0 - which is what lets every client idle while the compositor shows nothing - and never
// when it is 0.

using MobileGL::MG_Impl::EGLImpl::Wayland::FrameThrottle;

TEST(WaylandFrameThrottle, IntervalOneWaitsForTheCallbackOfThePreviousCommit) {
    FrameThrottle throttle;
    EXPECT_FALSE(throttle.MustWait(1)); // the first swap has nothing to wait for
    ASSERT_TRUE(throttle.WantsFrameRequest(1));
    throttle.Requested();
    EXPECT_TRUE(throttle.MustWait(1)); // the next swap waits ...
    EXPECT_FALSE(throttle.WantsFrameRequest(1)); // ... and asks for no second callback meanwhile
    throttle.Done();
    EXPECT_FALSE(throttle.MustWait(1)); // the compositor used the frame
    EXPECT_TRUE(throttle.WantsFrameRequest(1));
}

TEST(WaylandFrameThrottle, IntervalZeroNeitherAsksNorWaits) {
    FrameThrottle throttle;
    EXPECT_FALSE(throttle.WantsFrameRequest(0));
    EXPECT_FALSE(throttle.MustWait(0));
}

TEST(WaylandFrameThrottle, DroppingToIntervalZeroStopsWaitingOnAnOutstandingCallback) {
    FrameThrottle throttle;
    throttle.Requested();
    EXPECT_FALSE(throttle.MustWait(0));
    EXPECT_TRUE(throttle.Outstanding()); // still answered later, then requested again at interval 1
    EXPECT_FALSE(throttle.WantsFrameRequest(1));
    throttle.Done();
    EXPECT_TRUE(throttle.WantsFrameRequest(2));
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
