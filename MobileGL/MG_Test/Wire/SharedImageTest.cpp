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

#include <memory>
#include <string>

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
    EXPECT_EQ(SI::Allocate(16, 16, SI::FourCC('N', 'V', '1', '2'), why), nullptr);
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
