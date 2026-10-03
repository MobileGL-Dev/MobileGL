// MobileGL - MobileGL/MG_Test/Util/X11PresentTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// GLX presentation through DRI3 + Present (MG_Impl/GLXImpl/X11Present.h), driven with a fake X
// server and fake shared images: which path a window takes, how the images rotate through the X
// server's hands (PresentPixmap -> IdleNotify), how swaps are paced on CompleteNotify, what a resize
// does, and which failures send the window back to the readback path.

#include <MG_Impl/GLXImpl/X11Present.h>

#include <gtest/gtest.h>

#include <deque>
#include <functional>
#include <map>

#include <unistd.h>

using namespace MobileGL;
namespace XP = MobileGL::MG_Impl::GLXImpl::X11Present;
using MobileGL::MG_Util::Damage::Rect;
using MobileGL::MG_Util::Damage::Region;

namespace {
    struct Clock {
        Int64 now = 1000;
    };

    struct FakeImages final : XP::ImageSource {
        Uint64 nextId = 1;
        std::map<Uint64, std::pair<Uint32, Uint32>> live; // id -> size
        Vector<Uint64> released;
        Vector<std::pair<Uint64, Region>> copies;
        Bool failAllocate = false;
        Bool failCopy = false;

        Bool Allocate(Uint32 width, Uint32 height, Uint32 fourcc, XP::Image* out) override {
            if (failAllocate) return false;
            out->Id = nextId++;
            out->Fd = ::dup(0); // any descriptor: the chain closes it once the pixmap holds the memory
            out->Width = width;
            out->Height = height;
            out->Stride = width * 4;
            out->Fourcc = fourcc;
            live[out->Id] = {width, height};
            return true;
        }
        void Release(Uint64 id) override {
            live.erase(id);
            released.push_back(id);
        }
        Bool CopyFrame(Uint64 id, const Region& region) override {
            copies.emplace_back(id, region);
            return !failCopy;
        }
    };

    struct Presented {
        Uint32 pixmap = 0;
        Uint32 serial = 0;
        Uint32 options = 0;
        Uint64 targetMsc = 0;
        Region update;
    };

    struct FakeX final : XP::Connection {
        Clock& clock;
        std::deque<XP::Event> events;
        Uint32 nextPixmap = 0x400000;
        std::map<Uint32, Uint64> pixmaps; // pixmap -> image id
        Vector<Uint32> freed;
        Vector<Presented> presents;
        Bool refuseImport = false;
        Bool presentFailed = false;
        Uint32 lastImportDepth = 0;
        // Called whenever the chain waits with nothing queued: the X server's turn to answer.
        std::function<void(FakeX&)> serverTurn;

        explicit FakeX(Clock& c) : clock(c) {}

        Uint32 ImportPixmap(const XP::Image& image, Uint32 depth) override {
            EXPECT_GE(image.Fd, 0);
            lastImportDepth = depth;
            if (refuseImport) return 0;
            pixmaps[++nextPixmap] = image.Id;
            return nextPixmap;
        }
        void FreePixmap(Uint32 pixmap) override {
            freed.push_back(pixmap);
            pixmaps.erase(pixmap);
        }
        Bool PresentPixmap(Uint32 pixmap, Uint32 serial, Uint32 options, Uint64 targetMsc, const Region& update) override {
            presents.push_back({pixmap, serial, options, targetMsc, update});
            return true;
        }
        Bool PresentFailed() override { return presentFailed; }
        Bool PollEvent(XP::Event* out) override {
            if (events.empty()) return false;
            *out = events.front();
            events.pop_front();
            return true;
        }
        Bool WaitEvent(XP::Event* out, Int64 timeoutMs) override {
            if (events.empty() && serverTurn) serverTurn(*this);
            if (events.empty()) {
                clock.now += timeoutMs;
                return false;
            }
            clock.now += 1;
            return PollEvent(out);
        }
        void Flush() override {}

        void Idle(Uint32 pixmap) {
            XP::Event e;
            e.Type = XP::Event::Kind::Idle;
            e.Pixmap = pixmap;
            events.push_back(e);
        }
        void Complete(Uint32 serial, Uint64 msc) {
            XP::Event e;
            e.Type = XP::Event::Kind::Complete;
            e.Serial = serial;
            e.Msc = msc;
            events.push_back(e);
        }
        void Configure(Int32 w, Int32 h, Bool destroyed = false) {
            XP::Event e;
            e.Type = XP::Event::Kind::Configure;
            e.Width = w;
            e.Height = h;
            e.WindowDestroyed = destroyed;
            events.push_back(e);
        }
    };

    struct Rig {
        Clock clock;
        FakeImages images;
        FakeX x{clock};
        std::unique_ptr<XP::Chain> chain;
        explicit Rig(Uint32 depth = 24, Int32 w = 64, Int32 h = 48) {
            chain = std::make_unique<XP::Chain>(x, images, depth, w, h, [this] { return clock.now; });
        }
    };
} // namespace

TEST(X11PresentPath, Dri3WhenTheConnectionAndTheBackendHaveEverything) {
    XP::ConnectionCaps all;
    all.SharedImages = true;
    all.LocalConnection = true;
    all.Dri3Major = 1;
    all.Dri3Minor = 2;
    all.PresentMajor = 1;
    all.PresentMinor = 2;
    all.ShmMajor = 1;
    all.ShmMinor = 2;
    EXPECT_EQ(XP::SelectPath(all, 24, nullptr), XP::Path::Dri3Present);
    EXPECT_EQ(XP::SelectPath(all, 32, nullptr), XP::Path::Dri3Present);
    EXPECT_EQ(XP::SelectPath(all, 16, nullptr), XP::Path::ShmPutImage) << "no shared-image format for depth 16";
    EXPECT_EQ(XP::SelectPath(all, 24, "readback"), XP::Path::ShmPutImage);
    EXPECT_EQ(XP::SelectPath(all, 24, "putimage"), XP::Path::PutImage);

    XP::ConnectionCaps shmOnly = all; // Xwayland -shm: no glamor, so no DRI3
    shmOnly.Dri3Major = shmOnly.Dri3Minor = 0;
    EXPECT_EQ(XP::SelectPath(shmOnly, 24, nullptr), XP::Path::ShmPutImage);
    XP::ConnectionCaps monolith = all; // a backend without shared images
    monolith.SharedImages = false;
    EXPECT_EQ(XP::SelectPath(monolith, 24, nullptr), XP::Path::ShmPutImage);
    XP::ConnectionCaps oldShm = shmOnly; // MIT-SHM 1.1: SysV only
    oldShm.ShmMinor = 1;
    EXPECT_EQ(XP::SelectPath(oldShm, 24, nullptr), XP::Path::PutImage);
    XP::ConnectionCaps remote = all; // TCP: no descriptor travels
    remote.LocalConnection = false;
    EXPECT_EQ(XP::SelectPath(remote, 24, nullptr), XP::Path::PutImage);
    XP::ConnectionCaps noPresent = all;
    noPresent.PresentMajor = 0;
    EXPECT_EQ(XP::SelectPath(noPresent, 24, nullptr), XP::Path::ShmPutImage);

    EXPECT_EQ(XP::FourccForDepth(24), XP::kFourccXrgb8888);
    EXPECT_EQ(XP::FourccForDepth(32), XP::kFourccArgb8888);
    EXPECT_EQ(XP::FourccForDepth(30), 0u);
}

TEST(X11PresentChain, AFrameIsCopiedIntoAnImportedImageAndPresented) {
    Rig rig(32, 64, 48);
    ASSERT_TRUE(rig.chain->Present(Region::Full(), 0));
    ASSERT_EQ(rig.images.live.size(), 1u);
    const auto& [id, size] = *rig.images.live.begin();
    EXPECT_EQ(size, std::make_pair(64u, 48u));
    EXPECT_EQ(rig.x.lastImportDepth, 32u);
    ASSERT_EQ(rig.images.copies.size(), 1u);
    EXPECT_EQ(rig.images.copies[0].first, id);
    EXPECT_TRUE(rig.images.copies[0].second.IsFull());
    ASSERT_EQ(rig.x.presents.size(), 1u);
    EXPECT_EQ(rig.x.pixmaps.at(rig.x.presents[0].pixmap), id);
    EXPECT_EQ(rig.x.presents[0].options, XP::kPresentOptionAsync);
    EXPECT_EQ(rig.x.presents[0].serial, 1u);
    EXPECT_EQ(rig.chain->BusyBuffers(), 1u);
}

TEST(X11PresentChain, AnImageComesBackWithIdleNotifyAndIsReused) {
    Rig rig;
    ASSERT_TRUE(rig.chain->Present(Region::Full(), 0));
    const Uint32 first = rig.x.presents[0].pixmap;
    rig.x.Idle(first);
    ASSERT_TRUE(rig.chain->Present(Region::Full(), 0));
    EXPECT_EQ(rig.x.presents[1].pixmap, first) << "the idle image is taken before a new one is made";
    EXPECT_EQ(rig.chain->AllocatedBuffers(), 1u);
    // Held by the server: the next frame takes a second image, and a third.
    ASSERT_TRUE(rig.chain->Present(Region::Full(), 0));
    ASSERT_TRUE(rig.chain->Present(Region::Full(), 0));
    EXPECT_EQ(rig.chain->AllocatedBuffers(), 3u);
    EXPECT_EQ(rig.chain->BusyBuffers(), 3u);
    // Idle for an unknown pixmap is ignored.
    rig.x.Idle(0x1234);
    rig.x.Idle(rig.x.presents[2].pixmap);
    ASSERT_TRUE(rig.chain->Present(Region::Full(), 0));
    EXPECT_EQ(rig.x.presents[4].pixmap, rig.x.presents[2].pixmap);
    EXPECT_EQ(rig.chain->AllocatedBuffers(), 3u);
}

TEST(X11PresentChain, AllImagesHeldWaitsThenGrowsToFourThenDropsTheFrame) {
    Rig rig;
    for (int i = 0; i < 3; ++i) ASSERT_TRUE(rig.chain->Present(Region::Full(), 0));
    const Int64 before = rig.clock.now;
    ASSERT_TRUE(rig.chain->Present(Region::Full(), 0)) << "a fourth image after the idle wait";
    EXPECT_GE(rig.clock.now - before, XP::Chain::kIdleWaitMs);
    EXPECT_EQ(rig.chain->AllocatedBuffers(), XP::Chain::kMaxBuffers);
    EXPECT_FALSE(rig.chain->Present(Region::Full(), 0)) << "past the limit the frame is dropped";
    EXPECT_FALSE(rig.chain->Broken()) << "a slow server does not end the chain";
    EXPECT_EQ(rig.x.presents.size(), 4u);
    // The server answers while the chain waits: no fifth image, no dropped frame.
    rig.x.serverTurn = [](FakeX& x) { x.Idle(x.presents.front().pixmap); };
    ASSERT_TRUE(rig.chain->Present(Region::Full(), 0));
    EXPECT_EQ(rig.x.presents.back().pixmap, rig.x.presents.front().pixmap);
    EXPECT_EQ(rig.chain->AllocatedBuffers(), XP::Chain::kMaxBuffers);
}

TEST(X11PresentChain, IntervalOneWaitsForThePreviousCompletionAndTargetsTheNextMsc) {
    Rig rig;
    ASSERT_TRUE(rig.chain->Present(Region::Full(), 1));
    EXPECT_EQ(rig.x.presents[0].options, 0u);
    EXPECT_EQ(rig.x.presents[0].targetMsc, 0u) << "no MSC known yet";
    rig.x.serverTurn = [](FakeX& x) { x.Complete(1, 500); };
    ASSERT_TRUE(rig.chain->Present(Region::Full(), 1));
    EXPECT_EQ(rig.x.presents[1].targetMsc, 501u);
    EXPECT_TRUE(rig.x.events.empty());
    // Interval 2 targets two frames on.
    rig.x.serverTurn = [](FakeX& x) { x.Complete(2, 501); };
    ASSERT_TRUE(rig.chain->Present(Region::Full(), 2));
    EXPECT_EQ(rig.x.presents[2].targetMsc, 503u);
    // A server that never completes stalls a paced swap only so long.
    rig.x.serverTurn = nullptr;
    const Int64 before = rig.clock.now;
    rig.x.Idle(rig.x.presents[0].pixmap);
    ASSERT_TRUE(rig.chain->Present(Region::Full(), 1));
    EXPECT_GE(rig.clock.now - before, XP::Chain::kCompleteGiveUpMs);
    EXPECT_LT(rig.clock.now - before, XP::Chain::kCompleteGiveUpMs + XP::Chain::kIdleWaitMs);
}

TEST(X11PresentChain, IntervalZeroIsUnpacedUntilACompletionIsOverdue) {
    Rig rig;
    ASSERT_TRUE(rig.chain->Present(Region::Full(), 0));
    rig.x.Idle(rig.x.presents[0].pixmap);
    Int64 before = rig.clock.now;
    ASSERT_TRUE(rig.chain->Present(Region::Full(), 0));
    EXPECT_EQ(rig.clock.now, before) << "the completion is outstanding but not overdue";
    // Nothing of the window is shown: no completion for 250 ms.
    rig.clock.now += 250;
    rig.x.Idle(rig.x.presents[1].pixmap);
    before = rig.clock.now;
    ASSERT_TRUE(rig.chain->Present(Region::Full(), 0));
    EXPECT_GE(rig.clock.now - before, 1000) << "about one frame a second while nothing is shown";
    EXPECT_LE(rig.clock.now - before, 1001);
    // Completions come back: unpaced again.
    rig.x.Complete(1, 10);
    rig.x.Idle(rig.x.presents[2].pixmap);
    before = rig.clock.now;
    ASSERT_TRUE(rig.chain->Present(Region::Full(), 0));
    EXPECT_EQ(rig.clock.now, before);
}

TEST(X11PresentChain, AResizeRetiresOldImagesAsTheyComeBack) {
    Rig rig(24, 64, 48);
    const auto newSize = std::make_pair(128u, 96u);
    ASSERT_TRUE(rig.chain->Present(Region::Full(), 0));
    ASSERT_TRUE(rig.chain->Present(Region::Full(), 0));
    const Uint32 a = rig.x.presents[0].pixmap;
    const Uint32 b = rig.x.presents[1].pixmap;
    // Both with the server; a comes back, b does not.
    rig.x.Idle(a);
    rig.chain->Resize(128, 96);
    EXPECT_EQ(rig.chain->Width(), 128);
    EXPECT_TRUE(rig.x.freed.empty()) << "both were still with the server at the resize";
    ASSERT_TRUE(rig.chain->Present(Region::Full(), 0));
    const Uint32 c = rig.x.presents.back().pixmap;
    EXPECT_NE(c, a);
    EXPECT_NE(c, b);
    EXPECT_EQ(rig.images.live.at(rig.x.pixmaps.at(c)), newSize);
    EXPECT_EQ(rig.x.freed, Vector<Uint32>{a}) << "a came back at the old size and was replaced";
    EXPECT_EQ(rig.x.pixmaps.count(b), 1u) << "b is still with the server";
    // b comes back: the next frame that would take it gets a new-size image instead.
    rig.x.Idle(b);
    rig.x.Idle(c);
    ASSERT_TRUE(rig.chain->Present(Region::Full(), 0));
    EXPECT_EQ(rig.x.pixmaps.count(b), 0u);
    for (const auto& [id, size] : rig.images.live) EXPECT_EQ(size, newSize);
}

TEST(X11PresentChain, ConfigureNotifyReportsTheNewSizeOnce) {
    Rig rig;
    rig.x.Configure(300, 200);
    ASSERT_TRUE(rig.chain->Present(Region::Full(), 0));
    Int32 w = 0, h = 0;
    ASSERT_TRUE(rig.chain->TakeConfiguredSize(&w, &h));
    EXPECT_EQ(w, 300);
    EXPECT_EQ(h, 200);
    EXPECT_FALSE(rig.chain->TakeConfiguredSize(&w, &h));
}

TEST(X11PresentChain, PartialDamageIsFlippedIntoTheUpdateRegion) {
    Rig rig(24, 100, 100);
    const Rect gl{10, 0, 20, 5}; // bottom-left origin: the bottom rows
    ASSERT_TRUE(rig.chain->Present(Region::FromRects(&gl, 1), 0));
    ASSERT_FALSE(rig.x.presents[0].update.IsFull());
    ASSERT_EQ(rig.x.presents[0].update.Rects().size(), 1u);
    EXPECT_EQ(rig.x.presents[0].update.Rects()[0], (Rect{10, 95, 20, 5}));
    EXPECT_TRUE(rig.images.copies[0].second.IsFull()) << "a new image holds no frame yet: copied whole";
}

TEST(X11PresentChain, FailuresSendTheWindowBackToReadback) {
    {
        Rig rig;
        rig.x.refuseImport = true;
        EXPECT_FALSE(rig.chain->Present(Region::Full(), 0));
        EXPECT_TRUE(rig.chain->Broken());
        EXPECT_TRUE(rig.images.live.empty()) << "the refused image is released";
        EXPECT_FALSE(rig.chain->Present(Region::Full(), 0));
    }
    {
        Rig rig;
        rig.images.failAllocate = true;
        EXPECT_FALSE(rig.chain->Present(Region::Full(), 0));
        EXPECT_TRUE(rig.chain->Broken());
    }
    {
        Rig rig;
        rig.images.failCopy = true;
        EXPECT_FALSE(rig.chain->Present(Region::Full(), 0));
        EXPECT_TRUE(rig.chain->Broken());
        EXPECT_TRUE(rig.x.presents.empty()) << "nothing is presented that was not copied";
    }
    {
        Rig rig;
        ASSERT_TRUE(rig.chain->Present(Region::Full(), 0));
        rig.x.presentFailed = true;
        EXPECT_FALSE(rig.chain->Present(Region::Full(), 0));
        EXPECT_TRUE(rig.chain->Broken());
    }
    {
        Rig rig;
        ASSERT_TRUE(rig.chain->Present(Region::Full(), 0));
        rig.x.Configure(64, 48, /*destroyed=*/true);
        EXPECT_FALSE(rig.chain->Present(Region::Full(), 0));
        EXPECT_TRUE(rig.chain->Broken());
    }
    {
        Rig rig(16);
        EXPECT_TRUE(rig.chain->Broken()) << "depth 16 has no shared-image format";
    }
}

TEST(X11PresentChain, TheChainFreesEveryPixmapAndReleasesEveryImage) {
    Rig rig;
    for (int i = 0; i < 3; ++i) ASSERT_TRUE(rig.chain->Present(Region::Full(), 0));
    EXPECT_EQ(rig.images.live.size(), 3u);
    rig.chain.reset();
    EXPECT_TRUE(rig.images.live.empty());
    EXPECT_TRUE(rig.x.pixmaps.empty());
    EXPECT_EQ(rig.x.freed.size(), 3u);
}
