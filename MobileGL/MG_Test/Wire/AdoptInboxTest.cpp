// MobileGL - MobileGL/MG_Test/Wire/AdoptInboxTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P11 B2 (MG_Remote/CONTRACT-P11.md B2). Server::AdoptInbox - T0's Offers matched to their
// map_persistent records BY SEQ, never by arrival order - over both aux channels the product has:
// the in-process queue (inproc) and a real socket pair with SCM_RIGHTS (fork spawn / unix / fd:).
//
// Every case proves WHICH descriptor came back, not just that one did: each Offer's descriptor is
// a pipe whose far end carries the Offer's own seq, so a mix-up reads the wrong number.

#include <MG_Remote/Server/AdoptInbox.h>
#include <MG_Remote/Transport/AdoptT0.h>
#include <MG_Remote/Transport/InProcessTransport.h>
#include <MG_Remote/Transport/SocketTransport.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include <unistd.h>

using namespace MobileGL;
using namespace MobileGL::MG_Remote;
namespace T0 = MobileGL::MG_Remote::Transport::AdoptT0;

namespace {

    struct Channel {
        std::unique_ptr<Transport::ITransport> client;
        std::unique_ptr<Transport::ITransport> server;
    };

    Channel MakeChannel(bool socket) {
        Channel c;
        if (socket) {
            std::unique_ptr<Transport::SocketTransport> a, b;
            EXPECT_EQ(Transport::SocketTransport::CreatePair(a, b), MOBILEGL_OK);
            c.client = std::move(a);
            c.server = std::move(b);
        } else {
            std::unique_ptr<Transport::InProcessTransport> a, b;
            Transport::InProcessTransport::CreatePair(a, b);
            c.client = std::move(a);
            c.server = std::move(b);
        }
        return c;
    }

    // An Offer for `seq` whose descriptor is a pipe's read end; the pipe carries `seq` itself.
    void SendOffer(Transport::ITransport& client, std::uint64_t seq, std::uint32_t flags = T0::kOfferHasBuffer) {
        int p[2] = {-1, -1};
        ASSERT_EQ(::pipe(p), 0);
        ASSERT_EQ(::write(p[1], &seq, sizeof(seq)), static_cast<ssize_t>(sizeof(seq)));
        ::close(p[1]);
        T0::Offer offer{};
        offer.magic = T0::kOfferMagic;
        offer.version = T0::kOfferVersion;
        offer.seq = seq;
        offer.size = 4096;
        offer.flags = flags;
        ASSERT_EQ(client.ShareFd(p[0], MobileGLByteSpan{&offer, sizeof(offer)}), MOBILEGL_OK);
        ::close(p[0]);
    }

    // The seq the descriptor's pipe carries, and closes it.
    std::uint64_t SeqOf(int fd) {
        std::uint64_t seq = ~0ull;
        EXPECT_EQ(::read(fd, &seq, sizeof(seq)), static_cast<ssize_t>(sizeof(seq)));
        ::close(fd);
        return seq;
    }

    class AdoptInboxTest : public ::testing::TestWithParam<bool> {};

} // namespace

// In order: each Take finds its own Offer.
TEST_P(AdoptInboxTest, OffersInOrderAreTakenByTheirOwnRecords) {
    Channel c = MakeChannel(GetParam());
    Server::AdoptInbox inbox;
    inbox.Attach(c.server.get());
    SendOffer(*c.client, 10);
    SendOffer(*c.client, 11);
    for (std::uint64_t seq : {10ull, 11ull}) {
        int fd = -1;
        T0::Offer offer{};
        std::string why;
        ASSERT_EQ(inbox.Take(seq, 1000, &fd, &offer, why), Server::AdoptInbox::Outcome::Taken) << why;
        EXPECT_EQ(offer.seq, seq);
        EXPECT_EQ(SeqOf(fd), seq);
    }
    EXPECT_EQ(inbox.Kept(), 0u);
}

// OUT OF ORDER: the Offer for the later record arrives first. It is kept for that record, the
// earlier record still gets its own, and the later one is then served from what was kept.
TEST_P(AdoptInboxTest, AnOfferForALaterRecordIsKeptForItAndDoesNotAnswerAnEarlierOne) {
    Channel c = MakeChannel(GetParam());
    Server::AdoptInbox inbox;
    inbox.Attach(c.server.get());
    SendOffer(*c.client, 21);
    SendOffer(*c.client, 20);
    int fd = -1;
    T0::Offer offer{};
    std::string why;
    ASSERT_EQ(inbox.Take(20, 1000, &fd, &offer, why), Server::AdoptInbox::Outcome::Taken) << why;
    EXPECT_EQ(SeqOf(fd), 20u);
    EXPECT_EQ(inbox.Kept(), 1u);
    ASSERT_EQ(inbox.Take(21, 0, &fd, &offer, why), Server::AdoptInbox::Outcome::Taken) << why;
    EXPECT_EQ(SeqOf(fd), 21u);
    EXPECT_EQ(inbox.Kept(), 0u);
}

// LATE: the record is being waited for when its Offer arrives.
TEST_P(AdoptInboxTest, AnOfferThatArrivesDuringTheWaitIsTaken) {
    Channel c = MakeChannel(GetParam());
    Server::AdoptInbox inbox;
    inbox.Attach(c.server.get());
    std::thread late([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        SendOffer(*c.client, 30);
    });
    int fd = -1;
    T0::Offer offer{};
    std::string why;
    const auto start = std::chrono::steady_clock::now();
    const auto outcome = inbox.Take(30, 2000, &fd, &offer, why);
    const auto waited = std::chrono::steady_clock::now() - start;
    late.join();
    ASSERT_EQ(outcome, Server::AdoptInbox::Outcome::Taken) << why;
    EXPECT_EQ(SeqOf(fd), 30u);
    EXPECT_GE(waited, std::chrono::milliseconds(150));
}

// STALE: an Offer for a record that was already answered (its wait timed out) is closed and
// counted, and does not answer the record being waited for.
TEST_P(AdoptInboxTest, AnOfferForAnEarlierRecordIsStaleAndClosed) {
    Channel c = MakeChannel(GetParam());
    Server::AdoptInbox inbox;
    inbox.Attach(c.server.get());
    SendOffer(*c.client, 40);
    SendOffer(*c.client, 41);
    int fd = -1;
    T0::Offer offer{};
    std::string why;
    ASSERT_EQ(inbox.Take(41, 1000, &fd, &offer, why), Server::AdoptInbox::Outcome::Taken) << why;
    EXPECT_EQ(SeqOf(fd), 41u);
    EXPECT_EQ(inbox.StaleOffers(), 1u);
}

// MISSING: nothing comes - a bounded TimedOut with a reason, never a hang.
TEST_P(AdoptInboxTest, AnOfferThatNeverComesTimesOutWithinItsBound) {
    Channel c = MakeChannel(GetParam());
    Server::AdoptInbox inbox;
    inbox.Attach(c.server.get());
    int fd = -1;
    T0::Offer offer{};
    std::string why;
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(inbox.Take(50, 300, &fd, &offer, why), Server::AdoptInbox::Outcome::TimedOut);
    const auto waited = std::chrono::steady_clock::now() - start;
    EXPECT_EQ(fd, -1);
    EXPECT_NE(why.find("no store arrived"), std::string::npos) << why;
    EXPECT_LT(waited, std::chrono::milliseconds(2000));
}

// A descriptor whose sideband is not an Offer (another sender on the channel) is closed and
// counted, and the real Offer behind it still answers.
TEST_P(AdoptInboxTest, ADescriptorThatIsNotAnOfferIsSkipped) {
    Channel c = MakeChannel(GetParam());
    Server::AdoptInbox inbox;
    inbox.Attach(c.server.get());
    int p[2] = {-1, -1};
    ASSERT_EQ(::pipe(p), 0);
    const std::uint32_t junk = 7;
    ASSERT_EQ(c.client->ShareFd(p[0], MobileGLByteSpan{&junk, sizeof(junk)}), MOBILEGL_OK);
    ::close(p[0]);
    ::close(p[1]);
    SendOffer(*c.client, 60);
    int fd = -1;
    T0::Offer offer{};
    std::string why;
    ASSERT_EQ(inbox.Take(60, 1000, &fd, &offer, why), Server::AdoptInbox::Outcome::Taken) << why;
    EXPECT_EQ(SeqOf(fd), 60u);
    EXPECT_EQ(inbox.MalformedOffers(), 1u);
}

INSTANTIATE_TEST_SUITE_P(Channels, AdoptInboxTest, ::testing::Values(false, true),
                         [](const ::testing::TestParamInfo<bool>& info) {
                             return info.param ? std::string("socket") : std::string("inprocess");
                         });
