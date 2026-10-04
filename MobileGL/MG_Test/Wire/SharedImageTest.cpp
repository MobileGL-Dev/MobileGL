// MobileGL - MobileGL/MG_Test/Wire/SharedImageTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// Shared images (docs/Disaggregated/notes/anland/plan-ahb-dmabuf.md): the server's registry, a
// session's references, and the aux-socket inbox that carries an import's descriptor beside T0's
// Offers. On a host the image memory is a memfd, which is enough for everything here: what is
// under test is that a descriptor which travelled through SCM_RIGHTS still names its image.

#include <MG_Pipe/MGPipeTypes.h>
#include <MG_Remote/Server/AdoptInbox.h>
#include <MG_Remote/Server/SharedImageRegistry.h>
#include <MG_Remote/Transport/AdoptT0.h>
#include <MG_Remote/Transport/FdPassing.h>
#include <MG_Remote/Transport/SocketTransport.h>
#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <memory>
#include <vector>
#include <string>
#include <utility>

#include <sys/mman.h>
#include <unistd.h>

using namespace MobileGL;
using namespace MobileGL::MG_Remote;
namespace SI = MobileGL::MG_Remote::Server::SharedImages;

namespace {
    // A descriptor for `image` as a peer process would hold it: through a real socket pair.
    int ThroughSocket(int fd) {
        std::unique_ptr<Transport::SocketTransport> a, b;
        EXPECT_EQ(Transport::SocketTransport::CreatePair(a, b), MOBILEGL_OK);
        MG_Pipe::MGPSharedImageFdOffer offer{MG_Pipe::kMGPSharedImageFdMagic, 1, 1, 0, 0};
        EXPECT_EQ(a->ShareFd(fd, MobileGLByteSpan{&offer, sizeof(offer)}), MOBILEGL_OK);
        int received = -1;
        Uint8 sideband[Transport::FdPassing::kMaxSidebandBytes] = {};
        std::uint64_t size = 0;
        EXPECT_EQ(b->ReceiveFd(&received, MobileGLMutableByteSpan{sideband, sizeof(sideband)}, &size, 1000),
                  MOBILEGL_OK);
        return received;
    }
} // namespace

TEST(SharedImage, AnExportedDescriptorIdentifiesItsImageAfterCrossingASocket) {
    std::string why;
    SI::ImageRef image = SI::Allocate(64, 32, SI::kFourccAbgr8888, why);
    ASSERT_NE(image, nullptr) << why;
    EXPECT_EQ(image->Width, 64u);
    EXPECT_EQ(image->Height, 32u);
    EXPECT_GE(image->Stride, 64u * 4u);
    EXPECT_EQ(image->Modifier, SI::kModifierInvalid);
    const int peer = ThroughSocket(image->Fd);
    ASSERT_GE(peer, 0);
    SI::ImageRef found = SI::Identify(peer, why);
    ::close(peer);
    ASSERT_NE(found, nullptr) << why;
    EXPECT_EQ(found->Id, image->Id);
    EXPECT_EQ(SI::Find(image->Id), image);
}

// The negative control: a descriptor of the same kind this server never exported is refused.
TEST(SharedImage, AForeignDescriptorIsNotIdentified) {
    std::string why;
    SI::ImageRef image = SI::Allocate(16, 16, SI::kFourccXbgr8888, why);
    ASSERT_NE(image, nullptr) << why;
    const int foreign = ::memfd_create("not-a-shared-image", MFD_CLOEXEC);
    ASSERT_GE(foreign, 0);
    EXPECT_EQ(SI::Identify(foreign, why), nullptr);
    EXPECT_FALSE(why.empty());
    ::close(foreign);
}

TEST(SharedImage, AnImageDiesWithItsLastReferenceAndIsNoLongerIdentified) {
    std::string why;
    SI::ImageRef image = SI::Allocate(8, 8, SI::kFourccAbgr8888, why);
    ASSERT_NE(image, nullptr) << why;
    const Uint64 id = image->Id;
    const int kept = ::dup(image->Fd);
    const SizeT live = SI::LiveCount();
    image.reset();
    EXPECT_EQ(SI::LiveCount(), live - 1);
    EXPECT_EQ(SI::Find(id), nullptr);
    EXPECT_EQ(SI::Identify(kept, why), nullptr);
    ::close(kept);
}

TEST(SharedImage, UnsupportedFormatsAndSizesAreRefused) {
    std::string why;
    EXPECT_EQ(SI::Allocate(16, 16, SI::FourCC('R', 'G', '1', '6'), why), nullptr);
    // The ARGB orders are allocatable: the layout is the server's, and X ignores alpha.
    SI::ImageRef argb = SI::Allocate(16, 16, SI::kFourccArgb8888, why);
    ASSERT_NE(argb, nullptr) << why;
    EXPECT_EQ(argb->Fourcc, SI::kFourccArgb8888);
    EXPECT_TRUE(SI::FourccIgnoresAlpha(SI::kFourccXrgb8888));
    EXPECT_FALSE(SI::FourccIgnoresAlpha(SI::kFourccArgb8888));
    EXPECT_EQ(SI::Allocate(0, 16, SI::kFourccAbgr8888, why), nullptr);
    EXPECT_EQ(SI::Allocate(16, 1u << 20, SI::kFourccAbgr8888, why), nullptr);
}

// A session's holder keeps an image alive until its release or the session's end, and releases
// only its own reference: another holder's stays.
TEST(SharedImage, AHolderKeepsItsImagesUntilReleasedOrCleared) {
    std::string why;
    SI::SessionHolder producer, consumer;
    SI::ImageRef image = SI::Allocate(32, 32, SI::kFourccAbgr8888, why);
    ASSERT_NE(image, nullptr) << why;
    const Uint64 id = image->Id;
    producer.Hold(image);
    consumer.Hold(image);
    image.reset();
    EXPECT_NE(SI::Find(id), nullptr);
    EXPECT_TRUE(producer.Release(id));
    EXPECT_FALSE(producer.Release(id));
    EXPECT_NE(SI::Find(id), nullptr);
    EXPECT_NE(consumer.Get(id), nullptr);
    consumer.Clear();
    EXPECT_EQ(consumer.Count(), 0u);
    EXPECT_EQ(SI::Find(id), nullptr);
}

// The aux socket carries T0 Offers and shared-image descriptors at once; each Take finds its own
// kind for its own record, keeping the other for later.
TEST(SharedImage, TheInboxKeepsSharedImageAndT0DescriptorsApart) {
    std::unique_ptr<Transport::SocketTransport> client, server;
    ASSERT_EQ(Transport::SocketTransport::CreatePair(client, server), MOBILEGL_OK);
    Server::AdoptInbox inbox;
    inbox.Attach(server.get());

    std::string why;
    SI::ImageRef image = SI::Allocate(16, 16, SI::kFourccAbgr8888, why);
    ASSERT_NE(image, nullptr) << why;
    // The T0 Offer for the LATER record (6) arrives first; the import (5) is awaited first, as
    // records apply in order, and the Offer must be kept for its own record.
    int pipeFds[2] = {-1, -1};
    ASSERT_EQ(::pipe(pipeFds), 0);
    Transport::AdoptT0::Offer t0{};
    t0.magic = Transport::AdoptT0::kOfferMagic;
    t0.version = Transport::AdoptT0::kOfferVersion;
    t0.seq = 6;
    ASSERT_EQ(client->ShareFd(pipeFds[0], MobileGLByteSpan{&t0, sizeof(t0)}), MOBILEGL_OK);
    MG_Pipe::MGPSharedImageFdOffer offer{MG_Pipe::kMGPSharedImageFdMagic, 1, 5, 0, 0};
    ASSERT_EQ(client->ShareFd(image->Fd, MobileGLByteSpan{&offer, sizeof(offer)}), MOBILEGL_OK);
    ::close(pipeFds[0]);
    ::close(pipeFds[1]);

    int fd = -1;
    ASSERT_EQ(inbox.TakeSharedImageFd(5, 1000, &fd, why), Server::AdoptInbox::Outcome::Taken) << why;
    SI::ImageRef found = SI::Identify(fd, why);
    ::close(fd);
    ASSERT_NE(found, nullptr) << why;
    EXPECT_EQ(found->Id, image->Id);
    EXPECT_EQ(inbox.Kept(), 1u);

    Transport::AdoptT0::Offer taken{};
    ASSERT_EQ(inbox.Take(6, 0, &fd, &taken, why), Server::AdoptInbox::Outcome::Taken) << why;
    EXPECT_EQ(taken.seq, 6u);
    ::close(fd);
    // A shared-image record whose descriptor never comes is a bounded wait, not a hang.
    EXPECT_EQ(inbox.TakeSharedImageFd(7, 20, &fd, why), Server::AdoptInbox::Outcome::TimedOut);
}

// ---- synchronisation. A pipe stands in for a sync_file: its read end polls readable (signaled)
// once something is written to it, which is all the registry asks of a fence.
namespace {
    struct FakeFence {
        int read = -1;
        int write = -1;
        FakeFence() {
            int p[2];
            EXPECT_EQ(::pipe(p), 0);
            read = p[0];
            write = p[1];
        }
        ~FakeFence() {
            if (read >= 0) ::close(read);
            if (write >= 0) ::close(write);
        }
        int Take() { return std::exchange(read, -1); } // hands the read end over
        int Dup() const { return ::dup(read); }
        void Signal() { ASSERT_EQ(::write(write, "x", 1), 1); }
    };
} // namespace

TEST(SharedImageSync, AWriteMovesTheGenerationAndItsFenceIsHandedOutUntilItSignals) {
    std::string why;
    SI::ImageRef image = SI::Allocate(8, 8, SI::kFourccAbgr8888, why);
    ASSERT_NE(image, nullptr) << why;
    Uint64 generation = 99;
    EXPECT_EQ(SI::DupWriteFence(*image, &generation), -1);
    EXPECT_EQ(generation, 0u);

    FakeFence fence;
    const int pending = fence.Dup();
    SI::PublishWrite(*image, pending);
    const int copy = SI::DupWriteFence(*image, &generation);
    EXPECT_EQ(generation, 1u);
    ASSERT_GE(copy, 0);
    EXPECT_FALSE(SI::SyncFile::Signaled(copy));
    fence.Signal();
    EXPECT_TRUE(SI::SyncFile::Wait(copy, 1000));
    ::close(copy);
    // Signaled: nothing is left to wait for.
    EXPECT_EQ(SI::DupWriteFence(*image, &generation), -1);
    // A write that had already completed still moves the generation.
    SI::PublishWrite(*image, -1);
    EXPECT_EQ(SI::DupWriteFence(*image, &generation), -1);
    EXPECT_EQ(generation, 2u);
}

TEST(SharedImageSync, PendingReadsAreWaitedForAndSignaledOnesAreDropped) {
    std::string why;
    SI::ImageRef image = SI::Allocate(8, 8, SI::kFourccAbgr8888, why);
    ASSERT_NE(image, nullptr) << why;
    FakeFence first, second;
    SI::PublishRead(*image, first.Dup());
    SI::PublishRead(*image, second.Dup());
    Vector<int> pending = SI::DupPendingReadFences(*image);
    EXPECT_EQ(pending.size(), 2u);
    for (const int fd : pending) ::close(fd);
    EXPECT_FALSE(SI::WaitForReads(*image, 10));

    first.Signal();
    pending = SI::DupPendingReadFences(*image);
    EXPECT_EQ(pending.size(), 1u);
    for (const int fd : pending) ::close(fd);
    second.Signal();
    EXPECT_TRUE(SI::WaitForReads(*image, 10));
    EXPECT_TRUE(SI::DupPendingReadFences(*image).empty());
}

// A reader's frame fence reaches every image it noted, and only those.
TEST(SharedImageSync, AReadTrackerPublishesOneFrameToEveryImageItRead) {
    std::string why;
    SI::ImageRef a = SI::Allocate(8, 8, SI::kFourccAbgr8888, why);
    SI::ImageRef b = SI::Allocate(8, 8, SI::kFourccAbgr8888, why);
    SI::ImageRef untouched = SI::Allocate(8, 8, SI::kFourccAbgr8888, why);
    ASSERT_TRUE(a && b && untouched) << why;
    SI::ReadTracker tracker;
    tracker.NoteRead(a);
    tracker.NoteRead(b);
    tracker.NoteRead(a);
    EXPECT_EQ(tracker.Pending(), 2u);
    FakeFence frame;
    tracker.PublishFrame(frame.Dup());
    EXPECT_EQ(tracker.Pending(), 0u);
    EXPECT_FALSE(SI::WaitForReads(*a, 10));
    EXPECT_FALSE(SI::WaitForReads(*b, 10));
    EXPECT_TRUE(SI::WaitForReads(*untouched, 10));
    frame.Signal();
    EXPECT_TRUE(SI::WaitForReads(*a, 10));
    EXPECT_TRUE(SI::WaitForReads(*b, 10));
}

// Implicit sync by flush (glamor): what a session used since its last boundary is published as a
// write AND a read - a compositor sampling an X window's pixmap waits for the flush's fence, and the
// next writer of that pixmap (or of a client buffer the X server copied from) waits for it too.
TEST(SharedImageSync, AFlushPublishesEveryUsedImageAsAWriteAndARead) {
    std::string why;
    SI::ImageRef windowPixmap = SI::Allocate(8, 8, SI::kFourccXrgb8888, why);
    SI::ImageRef clientBuffer = SI::Allocate(8, 8, SI::kFourccXrgb8888, why);
    SI::ImageRef untouched = SI::Allocate(8, 8, SI::kFourccXrgb8888, why);
    ASSERT_TRUE(windowPixmap && clientBuffer && untouched) << why;
    SI::ReadTracker frame;
    frame.NoteRead(windowPixmap);
    frame.NoteRead(clientBuffer);
    FakeFence flush;
    frame.PublishFrameAsWrite(flush.Dup());
    EXPECT_EQ(frame.Pending(), 0u);

    Uint64 generation = 0;
    for (const SI::ImageRef& image : {windowPixmap, clientBuffer}) {
        const int write = SI::DupWriteFence(*image, &generation);
        EXPECT_EQ(generation, 1u) << "a reader must see the flush as a new write";
        ASSERT_GE(write, 0);
        EXPECT_FALSE(SI::SyncFile::Signaled(write));
        ::close(write);
        EXPECT_FALSE(SI::WaitForReads(*image, 10)) << "a writer must wait for the flush";
    }
    EXPECT_EQ(SI::DupWriteFence(*untouched, &generation), -1);
    EXPECT_EQ(generation, 0u);

    flush.Signal();
    EXPECT_EQ(SI::DupWriteFence(*windowPixmap, &generation), -1);
    EXPECT_TRUE(SI::WaitForReads(*windowPixmap, 10));
    EXPECT_TRUE(SI::WaitForReads(*clientBuffer, 10));

    // A completed flush (no fence) still moves the generation.
    frame.NoteRead(windowPixmap);
    frame.PublishFrameAsWrite(-1);
    EXPECT_EQ(SI::DupWriteFence(*windowPixmap, &generation), -1);
    EXPECT_EQ(generation, 2u);
}

// ---- YUV images (SharedImageYuv.h) ---------------------------------------------------------------

namespace {
    // A memfd holding a W x H NV12 frame with padded pitches and a gap before the chroma plane, the
    // way a decoder lays its buffers out; sample values are a function of position.
    struct ForeignFrame {
        int Fd = -1;
        SI::PlaneLayout Layout;
        SizeT Size = 0;
    };
    Uint8 LumaAt(Uint32 x, Uint32 y) { return static_cast<Uint8>(x * 3 + y * 7); }
    Uint8 CbAt(Uint32 x, Uint32 y) { return static_cast<Uint8>(100 + x + y * 5); }
    Uint8 CrAt(Uint32 x, Uint32 y) { return static_cast<Uint8>(200 - x - y * 3); }

    ForeignFrame MakeForeignNv12(Uint32 width, Uint32 height, Uint8 bias) {
        ForeignFrame frame;
        frame.Layout.Count = 2;
        frame.Layout.Pitch[0] = width + 32;
        frame.Layout.Pitch[1] = width + 64;
        frame.Layout.Offset[0] = 0;
        frame.Layout.Offset[1] = frame.Layout.Pitch[0] * (height + 8);
        frame.Size = frame.Layout.Offset[1] + static_cast<SizeT>(frame.Layout.Pitch[1]) * ((height + 1) / 2);
        frame.Fd = ::memfd_create("foreign-nv12", MFD_CLOEXEC);
        EXPECT_GE(frame.Fd, 0);
        EXPECT_EQ(::ftruncate(frame.Fd, static_cast<off_t>(frame.Size)), 0);
        auto* map = static_cast<Uint8*>(::mmap(nullptr, frame.Size, PROT_READ | PROT_WRITE, MAP_SHARED, frame.Fd, 0));
        EXPECT_NE(map, MAP_FAILED);
        for (Uint32 y = 0; y < height; ++y)
            for (Uint32 x = 0; x < width; ++x)
                map[y * frame.Layout.Pitch[0] + x] = static_cast<Uint8>(LumaAt(x, y) + bias);
        for (Uint32 y = 0; y < (height + 1) / 2; ++y) {
            for (Uint32 x = 0; x < (width + 1) / 2; ++x) {
                map[frame.Layout.Offset[1] + y * frame.Layout.Pitch[1] + 2 * x] = static_cast<Uint8>(CbAt(x, y) + bias);
                map[frame.Layout.Offset[1] + y * frame.Layout.Pitch[1] + 2 * x + 1] =
                    static_cast<Uint8>(CrAt(x, y) + bias);
            }
        }
        ::munmap(map, frame.Size);
        return frame;
    }

    // The image's own samples (a host image is a tight semi-planar memfd) equal the frame's.
    void ExpectImageHolds(const SI::Image& image, Uint8 bias) {
        const SizeT size = image.Plane1Offset + static_cast<SizeT>(image.Plane1Stride) * ((image.Height + 1) / 2);
        auto* map = static_cast<Uint8*>(::mmap(nullptr, size, PROT_READ, MAP_SHARED, image.Fd, 0));
        ASSERT_NE(map, MAP_FAILED);
        Uint32 wrong = 0;
        for (Uint32 y = 0; y < image.Height; ++y)
            for (Uint32 x = 0; x < image.Width; ++x)
                wrong += map[y * image.Stride + x] != static_cast<Uint8>(LumaAt(x, y) + bias);
        for (Uint32 y = 0; y < (image.Height + 1) / 2; ++y) {
            for (Uint32 x = 0; x < (image.Width + 1) / 2; ++x) {
                wrong += map[image.Plane1Offset + y * image.Plane1Stride + 2 * x] != static_cast<Uint8>(CbAt(x, y) + bias);
                wrong += map[image.Plane1Offset + y * image.Plane1Stride + 2 * x + 1] !=
                         static_cast<Uint8>(CrAt(x, y) + bias);
            }
        }
        EXPECT_EQ(wrong, 0u);
        ::munmap(map, size);
    }
} // namespace

TEST(SharedImageYuv, AnNv12ImageHasTwoPlanesInOneDescriptor) {
    std::string why;
    ASSERT_TRUE(SI::FourccSupported(SI::kFourccNv12));
    SI::ImageRef image = SI::Allocate(64, 32, SI::kFourccNv12, why);
    ASSERT_NE(image, nullptr) << why;
    EXPECT_TRUE(SI::FourccIsYuv(image->Fourcc));
    EXPECT_FALSE(image->IsForeign());
    EXPECT_GE(image->Stride, 64u);
    EXPECT_GE(image->Plane1Stride, 64u);
    EXPECT_GE(image->Plane1Offset, image->Stride * 32u);
    // Its own descriptor names it, like any image this server exported.
    const int peer = ThroughSocket(image->Fd);
    EXPECT_EQ(SI::Identify(peer, why), image);
    ::close(peer);
}

// THE CPU-COPY FALLBACK: a buffer this server never allocated becomes an image of its own, filled
// from the buffer's planes at each refresh; the same buffer imported again is the same image.
TEST(SharedImageYuv, AForeignBufferIsCopiedIntoAnImageOfItsOwn) {
    std::string why;
    ForeignFrame frame = MakeForeignNv12(48, 30, 0);
    ASSERT_GE(frame.Fd, 0);
    // Not one of the server's: Identify refuses it, the foreign import takes it.
    EXPECT_EQ(SI::Identify(frame.Fd, why), nullptr);
    const int peer = ThroughSocket(frame.Fd);
    SI::ImageRef image = SI::ImportForeignYuv(peer, 48, 30, SI::kFourccNv12, frame.Layout, why);
    ASSERT_NE(image, nullptr) << why;
    EXPECT_TRUE(image->IsForeign());
    ASSERT_TRUE(SI::RefreshForeign(*image, why)) << why;
    ExpectImageHolds(*image, 0);

    // New content in the same buffer shows at the next refresh.
    {
        ForeignFrame next = MakeForeignNv12(48, 30, 9);
        auto* source = static_cast<Uint8*>(::mmap(nullptr, next.Size, PROT_READ, MAP_SHARED, next.Fd, 0));
        auto* target = static_cast<Uint8*>(::mmap(nullptr, frame.Size, PROT_READ | PROT_WRITE, MAP_SHARED, frame.Fd, 0));
        std::memcpy(target, source, frame.Size);
        ::munmap(source, next.Size);
        ::munmap(target, frame.Size);
        ::close(next.Fd);
    }
    ASSERT_TRUE(SI::RefreshForeign(*image, why)) << why;
    ExpectImageHolds(*image, 9);

    // The same buffer with the same planes is the same image; other planes make another one.
    EXPECT_EQ(SI::ImportForeignYuv(frame.Fd, 48, 30, SI::kFourccNv12, frame.Layout, why), image);
    SI::PlaneLayout shifted = frame.Layout;
    shifted.Pitch[1] -= 2;
    SI::ImageRef other = SI::ImportForeignYuv(frame.Fd, 48, 30, SI::kFourccNv12, shifted, why);
    ASSERT_NE(other, nullptr) << why;
    EXPECT_NE(other, image);
    ::close(peer);
    ::close(frame.Fd);
}

TEST(SharedImageYuv, AForeignBufferTooSmallForItsPlanesIsRefused) {
    std::string why;
    ForeignFrame frame = MakeForeignNv12(48, 30, 0);
    SI::PlaneLayout beyond = frame.Layout;
    beyond.Offset[1] = static_cast<Uint32>(frame.Size);
    EXPECT_EQ(SI::ImportForeignYuv(frame.Fd, 48, 30, SI::kFourccNv12, beyond, why), nullptr);
    EXPECT_FALSE(why.empty());
    SI::PlaneLayout narrow = frame.Layout;
    narrow.Pitch[0] = 40; // shorter than a row
    EXPECT_EQ(SI::ImportForeignYuv(frame.Fd, 48, 30, SI::kFourccNv12, narrow, why), nullptr);
    SI::PlaneLayout onePlane = frame.Layout;
    onePlane.Count = 1;
    EXPECT_EQ(SI::ImportForeignYuv(frame.Fd, 48, 30, SI::kFourccNv12, onePlane, why), nullptr);
    EXPECT_EQ(SI::ImportForeignYuv(frame.Fd, 48, 30, SI::kFourccAbgr8888, frame.Layout, why), nullptr);
    ::close(frame.Fd);
}

// The plane copy into the layouts a platform's lock can hand out: semi-planar Cb first (copied by
// rows), Cr first and fully planar (sample by sample), and 16-bit samples.
TEST(SharedImageYuv, ThePlaneCopyFillsEveryDestinationLayout) {
    constexpr Uint32 W = 6, H = 4;
    Uint8 y[H][W], cbcr[H / 2][W];
    for (Uint32 r = 0; r < H; ++r)
        for (Uint32 c = 0; c < W; ++c) y[r][c] = static_cast<Uint8>(r * 10 + c);
    for (Uint32 r = 0; r < H / 2; ++r)
        for (Uint32 c = 0; c < W / 2; ++c) {
            cbcr[r][2 * c] = static_cast<Uint8>(100 + r * 10 + c);
            cbcr[r][2 * c + 1] = static_cast<Uint8>(200 + r * 10 + c);
        }
    const auto check = [&](const SI::Yuv::PlanarLayout& dst) {
        for (Uint32 r = 0; r < H; ++r)
            for (Uint32 c = 0; c < W; ++c) EXPECT_EQ(dst.Y.Data[r * dst.Y.RowStride + c * dst.Y.PixelStride], y[r][c]);
        for (Uint32 r = 0; r < H / 2; ++r)
            for (Uint32 c = 0; c < W / 2; ++c) {
                EXPECT_EQ(dst.Cb.Data[r * dst.Cb.RowStride + c * dst.Cb.PixelStride], cbcr[r][2 * c]);
                EXPECT_EQ(dst.Cr.Data[r * dst.Cr.RowStride + c * dst.Cr.PixelStride], cbcr[r][2 * c + 1]);
            }
    };
    std::vector<Uint8> luma(16 * H), chroma(16 * H);
    SI::Yuv::PlanarLayout nv12{{luma.data(), 16, 1}, {chroma.data(), 16, 2}, {chroma.data() + 1, 16, 2}};
    ASSERT_TRUE(SI::Yuv::CopySemiPlanar(&y[0][0], W, &cbcr[0][0], W, W, H, 1, nv12));
    check(nv12);
    SI::Yuv::PlanarLayout nv21{{luma.data(), 16, 1}, {chroma.data() + 1, 16, 2}, {chroma.data(), 16, 2}};
    ASSERT_TRUE(SI::Yuv::CopySemiPlanar(&y[0][0], W, &cbcr[0][0], W, W, H, 1, nv21));
    check(nv21);
    std::vector<Uint8> cb(8 * H), cr(8 * H);
    SI::Yuv::PlanarLayout planar{{luma.data(), 16, 1}, {cb.data(), 8, 1}, {cr.data(), 8, 1}};
    ASSERT_TRUE(SI::Yuv::CopySemiPlanar(&y[0][0], W, &cbcr[0][0], W, W, H, 1, planar));
    check(planar);
    SI::Yuv::PlanarLayout missing{{luma.data(), 16, 1}, {nullptr, 8, 1}, {cr.data(), 8, 1}};
    EXPECT_FALSE(SI::Yuv::CopySemiPlanar(&y[0][0], W, &cbcr[0][0], W, W, H, 1, missing));

    // 16-bit samples (P010): two bytes each, Cb Cr pairs of four.
    Uint16 y16[H][W], c16[H / 2][W];
    for (Uint32 r = 0; r < H; ++r)
        for (Uint32 c = 0; c < W; ++c) y16[r][c] = static_cast<Uint16>((r * 10 + c) << 6);
    for (Uint32 r = 0; r < H / 2; ++r)
        for (Uint32 c = 0; c < W; ++c) c16[r][c] = static_cast<Uint16>((500 + r * 10 + c) << 6);
    std::vector<Uint16> luma16(16 * H), chroma16(16 * H);
    SI::Yuv::PlanarLayout p010{{reinterpret_cast<Uint8*>(luma16.data()), 32, 2},
                               {reinterpret_cast<Uint8*>(chroma16.data()), 32, 4},
                               {reinterpret_cast<Uint8*>(chroma16.data()) + 2, 32, 4}};
    ASSERT_TRUE(SI::Yuv::CopySemiPlanar(reinterpret_cast<const Uint8*>(&y16[0][0]), W * 2,
                                        reinterpret_cast<const Uint8*>(&c16[0][0]), W * 2, W, H, 2, p010));
    for (Uint32 r = 0; r < H; ++r)
        for (Uint32 c = 0; c < W; ++c) EXPECT_EQ(luma16[r * 16 + c], y16[r][c]);
    for (Uint32 r = 0; r < H / 2; ++r)
        for (Uint32 c = 0; c < W; ++c) EXPECT_EQ(chroma16[r * 16 + c], c16[r][c]);
}

// The conversion an import's hints select: BT.709 narrow-range codes of saturated colours decode
// to those colours, and the defaults are BT.601 narrow.
TEST(SharedImageYuv, TheConversionDecodesTheHintedColourSpace) {
    const auto decode = [](const SI::Yuv::Conversion& c, double y, double cb, double cr, double max) {
        const double in[3] = {y / max - c.Offset[0], cb / max - c.Offset[1], cr / max - c.Offset[2]};
        std::array<double, 3> rgb{};
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col) rgb[row] += c.Matrix[col * 3 + row] * in[col];
        return rgb;
    };
    SI::Yuv::Hints bt709;
    bt709.ColorSpace = SI::Yuv::kEglItuRec709;
    bt709.Range = SI::Yuv::kEglYuvNarrowRange;
    const auto c709 = SI::Yuv::ConversionFor(bt709, 255.0);
    // Red, green, blue and white in BT.709 narrow range (8-bit codes).
    const double codes[4][3] = {{63, 102, 240}, {173, 42, 26}, {32, 240, 118}, {235, 128, 128}};
    const double want[4][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 1}};
    for (int i = 0; i < 4; ++i) {
        const auto rgb = decode(c709, codes[i][0], codes[i][1], codes[i][2], 255.0);
        for (int k = 0; k < 3; ++k) EXPECT_NEAR(rgb[k], want[i][k], 0.02) << "colour " << i << " channel " << k;
    }
    // Full range: Y passes through.
    SI::Yuv::Hints full;
    full.Range = SI::Yuv::kEglYuvFullRange;
    const auto cFull = SI::Yuv::ConversionFor(full, 255.0);
    const auto grey = decode(cFull, 128, 128, 128, 255.0);
    for (int k = 0; k < 3; ++k) EXPECT_NEAR(grey[k], 128.0 / 255.0, 0.005);
    EXPECT_EQ(SI::Yuv::Hints{}.EffectiveColorSpace(), SI::Yuv::kEglItuRec601);
    EXPECT_FALSE(SI::Yuv::Hints{}.FullRange());
}
