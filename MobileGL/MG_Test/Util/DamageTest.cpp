// MobileGL - MobileGL/MG_Test/Util/DamageTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The partial-present bookkeeping (MG_Util/Damage/Damage.h): the y-flip between EGL's bottom-left
// rectangles and a top-down buffer's, the clamping and merging that fits damage into a record, the
// per-buffer damage a rotating set of shared images has missed, and the per-image buffer age.

#include <gtest/gtest.h>

#include <MG_Util/Damage/Damage.h>

using namespace MobileGL;
using namespace MobileGL::MG_Util::Damage;

namespace {
    // Every pixel of `inner` lies inside some rectangle of `region` (or the region is Full).
    Bool Covers(const Region& region, const Rect& inner, Int32 width, Int32 height) {
        if (region.IsFull()) return true;
        const Rect clipped = Clip(inner, width, height);
        for (Int32 y = clipped.Y; y < clipped.Y + clipped.Height; ++y) {
            for (Int32 x = clipped.X; x < clipped.X + clipped.Width; ++x) {
                Bool inside = false;
                for (const Rect& r : region.Rects()) {
                    if (x >= r.X && x < r.X + r.Width && y >= r.Y && y < r.Y + r.Height) {
                        inside = true;
                        break;
                    }
                }
                if (!inside) return false;
            }
        }
        return true;
    }

    Region Of(std::initializer_list<Rect> rects) { return Region::FromRects(rects.begin(), rects.size()); }
} // namespace

// EGL damage (origin bottom-left) -> wl_surface.damage_buffer / shared-image rows (origin top-left).
TEST(Damage, FlipYMovesTheOriginBetweenBottomAndTop) {
    // A 30x40 rectangle 20 rows above the bottom of a 100-row surface starts 40 rows below its top.
    EXPECT_EQ(FlipY({10, 20, 30, 40}, 100), (Rect{10, 40, 30, 40}));
    // The bottom row is the last row from the top, and the top row the first.
    EXPECT_EQ(FlipY({0, 0, 5, 1}, 100), (Rect{0, 99, 5, 1}));
    EXPECT_EQ(FlipY({0, 99, 5, 1}, 100), (Rect{0, 0, 5, 1}));
    // Its own inverse.
    const Rect r{7, 13, 21, 8};
    EXPECT_EQ(FlipY(FlipY(r, 77), 77), r);
    // A Region flips every rectangle and leaves Full alone.
    EXPECT_EQ(Of({{0, 0, 10, 10}}).FlippedY(50), Of({{0, 40, 10, 10}}));
    EXPECT_TRUE(Region::Full().FlippedY(50).IsFull());
}

TEST(Damage, ClipKeepsTheSurfacePartOnly) {
    EXPECT_EQ(Clip({-5, -5, 10, 10}, 100, 100), (Rect{0, 0, 5, 5}));
    EXPECT_EQ(Clip({95, 90, 10, 20}, 100, 100), (Rect{95, 90, 5, 10}));
    EXPECT_TRUE(Clip({100, 0, 10, 10}, 100, 100).Empty());
    EXPECT_TRUE(Clip({0, -20, 10, 10}, 100, 100).Empty());
}

TEST(Damage, EglRectsWithNoneMeanTheWholeSurface) {
    EXPECT_TRUE(Region::FromEglRects<Int32>(nullptr, 0).IsFull());
    const Int32 rects[] = {1, 2, 3, 4, 10, 20, 30, 40};
    EXPECT_TRUE(Region::FromEglRects(rects, 0).IsFull());
    const Region two = Region::FromEglRects(rects, 2);
    ASSERT_FALSE(two.IsFull());
    EXPECT_EQ(two.Rects(), (Vector<Rect>{{1, 2, 3, 4}, {10, 20, 30, 40}}));
}

TEST(Damage, NormalizeClipsDropsAndPromotesToFull) {
    Region region = Of({{-10, -10, 20, 20}, {200, 200, 5, 5}, {50, 50, 0, 3}});
    region.Normalize(100, 100);
    EXPECT_EQ(region, Of({{0, 0, 10, 10}}));

    Region outside = Of({{200, 200, 5, 5}});
    outside.Normalize(100, 100);
    EXPECT_TRUE(outside.IsEmpty()) << "damage entirely off the surface damages nothing";

    Region whole = Of({{10, 10, 5, 5}, {-1, -1, 102, 102}});
    whole.Normalize(100, 100);
    EXPECT_TRUE(whole.IsFull()) << "a rectangle covering the surface is the whole surface";
}

TEST(Damage, NormalizeMergesDownToTheLimitAndStillCoversEverything) {
    Vector<Rect> input;
    for (Int32 i = 0; i < 40; ++i) input.push_back({(i * 37) % 900, (i * 53) % 600, 9 + i % 5, 7 + i % 3});
    Region region = Region::FromRects(input.data(), input.size());
    region.Normalize(1000, 700, kMaxRects);
    ASSERT_FALSE(region.IsFull());
    EXPECT_LE(region.Rects().size(), kMaxRects);
    for (const Rect& r : input) EXPECT_TRUE(Covers(region, r, 1000, 700)) << r.X << "," << r.Y;

    // Two neighbours and one far away, limit 2: the neighbours merge, the far one stays apart.
    Region three = Of({{0, 0, 10, 10}, {10, 0, 10, 10}, {500, 500, 10, 10}});
    three.Normalize(1000, 1000, 2);
    EXPECT_EQ(three, Of({{0, 0, 20, 10}, {500, 500, 10, 10}}));
}

// Three shared images rotate; each write must bring over what that image missed.
TEST(Damage, EachBufferCopiesTheDamageItMissedSinceItsLastWrite) {
    constexpr Int32 W = 200, H = 100;
    BufferDamageTracker tracker(3);
    const Region a = Of({{0, 0, 10, 10}});
    const Region b = Of({{20, 0, 10, 10}});
    const Region c = Of({{40, 0, 10, 10}});
    const Region d = Of({{60, 0, 10, 10}});
    const Region e = Of({{80, 0, 10, 10}});

    // A buffer never written has unknown contents: its first write is the whole frame.
    EXPECT_TRUE(tracker.TakeForWrite(0, a, W, H).IsFull());
    EXPECT_TRUE(tracker.TakeForWrite(1, b, W, H).IsFull());
    EXPECT_TRUE(tracker.TakeForWrite(2, c, W, H).IsFull());

    // Buffer 0 last held frame A; frames B and C went elsewhere, D comes now.
    Region copy0 = tracker.TakeForWrite(0, d, W, H);
    ASSERT_FALSE(copy0.IsFull());
    for (const Region* r : {&b, &c, &d}) EXPECT_TRUE(Covers(copy0, r->Rects()[0], W, H));
    EXPECT_FALSE(Covers(copy0, a.Rects()[0], W, H)) << "A is already in buffer 0";

    Region copy1 = tracker.TakeForWrite(1, e, W, H);
    for (const Region* r : {&c, &d, &e}) EXPECT_TRUE(Covers(copy1, r->Rects()[0], W, H));
    EXPECT_FALSE(Covers(copy1, b.Rects()[0], W, H));

    // A reallocated buffer starts over.
    tracker.Invalidate(2);
    EXPECT_TRUE(tracker.TakeForWrite(2, a, W, H).IsFull());

    // A full-surface frame makes every other buffer copy everything next time.
    EXPECT_TRUE(tracker.TakeForWrite(0, Region::Full(), W, H).IsFull());
    EXPECT_TRUE(tracker.TakeForWrite(1, a, W, H).IsFull());
    EXPECT_TRUE(tracker.TakeForWrite(2, a, W, H).IsFull());
    // ... and once caught up, only the frames' own damage.
    EXPECT_EQ(tracker.TakeForWrite(0, b, W, H), Of({{0, 0, 10, 10}, {20, 0, 10, 10}}))
        << "buffer 0 missed A twice (buffers 1 and 2) and takes B";
}

TEST(Damage, OneBufferInUseCopiesOnlyEachFramesDamage) {
    BufferDamageTracker tracker(3);
    EXPECT_TRUE(tracker.TakeForWrite(0, Of({{0, 0, 4, 4}}), 64, 64).IsFull());
    EXPECT_EQ(tracker.TakeForWrite(0, Of({{8, 8, 4, 4}}), 64, 64), Of({{8, 8, 4, 4}}));
    EXPECT_EQ(tracker.TakeForWrite(0, Of({{16, 8, 4, 4}}), 64, 64), Of({{16, 8, 4, 4}}));
    // Nothing changed: nothing to copy.
    EXPECT_TRUE(tracker.TakeForWrite(0, Region(), 64, 64).IsEmpty());
}

TEST(Damage, MissedDamageStaysBoundedAndClipped) {
    BufferDamageTracker tracker(2);
    // Buffer 0 is up to date with a full frame; the next fifty frames all go to buffer 1.
    (void)tracker.TakeForWrite(1, Region::Full(), 100, 100);
    (void)tracker.TakeForWrite(0, Region::Full(), 100, 100);
    Vector<Rect> written;
    for (Int32 i = 0; i < 50; ++i) {
        const Rect r{(i * 13) % 90, (i * 29) % 90, 3, 3};
        written.push_back(r);
        (void)tracker.TakeForWrite(1, Of({r, {150, 150, 5, 5}}), 100, 100);
    }
    const Region copy = tracker.TakeForWrite(0, Region(), 100, 100);
    ASSERT_FALSE(copy.IsFull());
    EXPECT_LE(copy.Rects().size(), kMaxRects);
    for (const Rect& r : copy.Rects()) EXPECT_FALSE(Clip(r, 100, 100) != r) << "clipped to the surface";
    for (const Rect& r : written) EXPECT_TRUE(Covers(copy, r, 100, 100));
}

// EGL_EXT_buffer_age per swapchain image.
TEST(Damage, SwapchainAgeCountsPresentsSinceTheImageWasShown) {
    SwapchainAgeTracker ages;
    ages.Reset(2);
    EXPECT_EQ(ages.AgeOf(0), 0) << "never presented";
    ages.OnPresented(0, true);
    EXPECT_EQ(ages.AgeOf(1), 0);
    ages.OnPresented(1, true);
    EXPECT_EQ(ages.AgeOf(0), 2) << "double buffering: the image shown two presents ago";
    ages.OnPresented(0, true);
    EXPECT_EQ(ages.AgeOf(1), 2);

    // Triple buffering, in order.
    ages.Reset(3);
    for (SizeT i = 0; i < 3; ++i) ages.OnPresented(i, true);
    EXPECT_EQ(ages.AgeOf(0), 3);
    ages.OnPresented(0, true);
    EXPECT_EQ(ages.AgeOf(1), 3);
    // The same image again (a chain that hands one image back at once).
    EXPECT_EQ(ages.AgeOf(0), 1);
}

TEST(Damage, SwapchainAgeIsUnknownWhenContentWasNotKeptOrTheChainChanged) {
    SwapchainAgeTracker ages;
    ages.Reset(2);
    ages.OnPresented(0, false);
    ages.OnPresented(1, true);
    EXPECT_EQ(ages.AgeOf(0), 0) << "presented without keeping its content";
    EXPECT_EQ(ages.AgeOf(1), 1);

    ages.OnPresented(0, true);
    ages.OnFrameDropped();
    EXPECT_EQ(ages.AgeOf(0), 0) << "a dropped frame breaks the count the application keeps";
    EXPECT_EQ(ages.AgeOf(1), 0);

    ages.OnPresented(0, true);
    ages.OnPresented(1, true);
    ages.Reset(3);
    EXPECT_EQ(ages.AgeOf(0), 0) << "a rebuilt chain";
    EXPECT_EQ(ages.AgeOf(1), 0);
    EXPECT_EQ(ages.AgeOf(7), 0) << "an index the chain does not have";
}
