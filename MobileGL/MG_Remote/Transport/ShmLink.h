// SPDX-License-Identifier: LGPL-3.0-only
#pragma once
#include "ILink.h"
#include "SessionRings.h"
#include <vector>
namespace MobileGL::MG_Remote::Transport {
    // Owns the mappings and every physical endpoint. Shared and private mirrored
    // storage use identical ring algorithms; only delivery belongs to the link.
    class ShmLink final : public ILink {
    public:
        ShmLink() = default;
        ~ShmLink() override = default;
        SessionSegments& Memory() override { return m_memory; }
        RingProducer& CommandsOut() override { return m_commandsOut; }
        RingConsumer& CommandsIn() override { return m_commandsIn; }
        EventRingProducer& EventsOut() override { return m_eventsOut; }
        EventRingConsumer& EventsIn() override { return m_eventsIn; }
        void InitializeEndpoints() override;
        void BindDoorbells(Doorbell* consumer, Doorbell* producer) override;
        void SetRole(TransportRoleTag role) { m_role = role; }
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
        MobileGLResult Flush() override { return MOBILEGL_OK; }
        MobileGLResult FlushProgress() override { return MOBILEGL_OK; }
        bool Attached() const override { return m_memory.Valid(); }
        void Detach() override;
        TransportRoleTag Role() const override { return m_role; }

    private:
        SessionSegments m_memory;
        RingProducer m_commandsOut;
        RingConsumer m_commandsIn;
        EventRingProducer m_eventsOut;
        EventRingConsumer m_eventsIn;
        Doorbell* m_consumer = nullptr;
        Doorbell* m_producer = nullptr;
        TransportRoleTag m_role = TransportRoleTag::ClientProducer;
        LinkArena m_recordArena{}, m_eventArena{};
        LinkCursor m_recordCursor{}, m_eventCursor{};
        std::vector<std::uint8_t> m_reply;
    };
} // namespace MobileGL::MG_Remote::Transport
