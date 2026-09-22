// SPDX-License-Identifier: LGPL-3.0-only
#pragma once
#include "ILink.h"
#include <memory>

namespace MobileGL::MG_Remote::Transport {
    class SessionSegments;
    // One reader owns receive/reassembly. Writers serialize complete frames and use
    // the caller thread, so both directions can always drain a blocked peer writer.
    class StreamLink final : public ILink {
    public:
        StreamLink();
        ~StreamLink() override;
        MobileGLResult Attach(int dataFd, SessionSegments& mirrors, TransportRoleTag role);
        MobileGLResult AttachOwned(int dataFd, const struct SessionSegmentSizes& sizes, TransportRoleTag role);
        SessionSegments& Memory() override;
        void InitializeEndpoints() override;
        RingProducer& CommandsOut() override;
        RingConsumer& CommandsIn() override;
        EventRingProducer& EventsOut() override;
        EventRingConsumer& EventsIn() override;
        LinkCapabilities Capabilities() const override;
        LinkProgress* Progress() override;
        LinkEventFlags* EventFlags() override;
        LinkArena* RecordArena() override;
        LinkCursor* RecordCursor() override;
        MobileGLResult StageBytes(std::uint64_t, LinkSpan*, void**) override;
        MobileGLResult ResolveSpan(LinkSegment, LinkSpan, const void**) override;
        MobileGLResult PostReply(std::uint64_t, std::int32_t, const void*, std::uint64_t) override;
        MobileGLResult ReadReply(std::uint64_t, std::int32_t*, const void**, std::uint64_t*) override;
        LinkArena* EventArena() override;
        LinkCursor* EventCursor() override;
        std::uint64_t EventPublishedHead() const override;
        MobileGLResult WaitForEventDelivery(std::uint64_t head, std::uint32_t timeoutMs) override;
        Doorbell& ConsumerBell() override;
        Doorbell& ProducerBell() override;
        MobileGLResult Flush() override;
        MobileGLResult FlushProgress() override;
        void NoteStage(LinkSpan span) override;
        void ProgressChanged() override;
        bool PeerHungUp() const override;
        bool Attached() const override;
        void Detach() override;
        TransportRoleTag Role() const override;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
} // namespace MobileGL::MG_Remote::Transport
