// SPDX-License-Identifier: LGPL-3.0-only
#include "ShmLink.h"
namespace MobileGL::MG_Remote::Transport {
    LinkSignals ILink::Signals() {
        auto* c = Memory().CmdControl();
        return {&c->cmdHead, &c->submittedSeq, &c->consumerParked, &c->producerParked, &c->eventRingFull};
    }
    std::uint64_t ILink::SegmentSize(LinkSegment segment) {
        auto& m = Memory();
        switch (segment) {
        case LinkSegment::Cmd:
            return m.CmdRingCapacity();
        case LinkSegment::Stage:
            return m.StageBytes();
        case LinkSegment::Reply:
            return m.ReplyBytes();
        case LinkSegment::Event:
            return m.AnnouncedSize(SessionSegmentSlot::Event);
        default:
            return 0;
        }
    }
    std::unique_ptr<ILink> CreateSharedLink(TransportRoleTag role) {
        auto link = std::make_unique<ShmLink>();
        link->SetRole(role);
        return link;
    }
    void ShmLink::InitializeEndpoints() {
        auto* c = m_memory.CmdControl();
        m_commandsOut = RingProducer(c, m_memory.CmdRingBase(), m_memory.CmdRingCapacity(), RingCursorSet::Cmd);
        m_commandsIn = RingConsumer(c, m_memory.CmdRingBase(), m_memory.CmdRingCapacity(), RingCursorSet::Cmd);
        m_eventsOut =
            EventRingProducer(m_memory.EventControl(), c, m_memory.EventRingBase(), m_memory.EventRingCapacity());
        m_eventsIn = EventRingConsumer(m_memory.EventControl(), c, m_memory.EventRingBase(),
                                       m_memory.EventRingCapacity(), m_memory.EventSegmentBase());
        if (m_role == TransportRoleTag::ServerConsumer) {
            ReplySlotPool pool(m_memory.ReplyBase(), m_memory.ReplyBytes(), m_memory.ReplySlotCount());
            pool.Clear();
        }
    }
    void ShmLink::BindDoorbells(Doorbell* c, Doorbell* p) {
        m_consumer = c;
        m_producer = p;
    }
    LinkCapabilities ShmLink::Capabilities() const {
        const ReplySlotPool replies(m_memory.ReplyBase(), m_memory.ReplyBytes(), m_memory.ReplySlotCount());
        return {true, true, true, m_memory.CmdRingCapacity() / 2, replies.MaxReplyBytes()};
    }
    LinkProgress* ShmLink::Progress() {
        return m_memory.Valid() ? &m_memory.CmdControl()->Progress : nullptr;
    }
    LinkEventFlags* ShmLink::EventFlags() {
        return m_memory.Valid() ? reinterpret_cast<LinkEventFlags*>(&m_memory.CmdControl()->eventRingFull) : nullptr;
    }
    LinkArena* ShmLink::RecordArena() {
        m_recordArena = {m_memory.CmdRingBase(), m_memory.CmdRingCapacity(), m_memory.CmdRingCapacity() - 1,
                         m_commandsOut.LocalHead()};
        return &m_recordArena;
    }
    LinkCursor* ShmLink::RecordCursor() {
        m_recordCursor = {m_memory.CmdRingBase(), m_memory.CmdRingCapacity(), m_memory.CmdRingCapacity() - 1,
                          m_commandsIn.LocalTail()};
        return &m_recordCursor;
    }
    LinkArena* ShmLink::EventArena() {
        m_eventArena = {m_memory.EventRingBase(), m_memory.EventRingCapacity(), m_memory.EventRingCapacity() - 1,
                        m_eventsOut.Ring().LocalHead()};
        return &m_eventArena;
    }
    LinkCursor* ShmLink::EventCursor() {
        m_eventCursor = {m_memory.EventRingBase(), m_memory.EventRingCapacity(), m_memory.EventRingCapacity() - 1,
                         m_eventsIn.Ring().LocalTail()};
        return &m_eventCursor;
    }
    MobileGLResult ShmLink::StageBytes(std::uint64_t, LinkSpan*, void**) {
        return MOBILEGL_ERR_UNSUPPORTED;
    }
    MobileGLResult ShmLink::ResolveSpan(LinkSegment segment, LinkSpan span, const void** out) {
        if (!out || !Attached()) return MOBILEGL_ERR_NOT_INITIALIZED;
        const void* base = nullptr;
        std::uint64_t size = 0;
        switch (segment) {
        case LinkSegment::Cmd:
            base = m_memory.CmdRingBase();
            size = m_memory.CmdRingCapacity();
            break;
        case LinkSegment::Stage:
            base = m_memory.StageBase();
            size = m_memory.StageBytes();
            break;
        case LinkSegment::Reply:
            base = m_memory.ReplyBase();
            size = m_memory.ReplyBytes();
            break;
        case LinkSegment::Event:
            base = m_memory.EventSegmentBase();
            size = m_memory.AnnouncedSize(SessionSegmentSlot::Event);
            break;
        default:
            return MOBILEGL_ERR_UNSUPPORTED;
        }
        if (span.Offset > size || span.Size > size - span.Offset) return MOBILEGL_ERR_PROTOCOL_MISMATCH;
        *out = static_cast<const std::uint8_t*>(base) + span.Offset;
        return MOBILEGL_OK;
    }
    MobileGLResult ShmLink::PostReply(std::uint64_t seq, std::int32_t status, const void* payload, std::uint64_t size) {
        ReplySlotPool replies(m_memory.ReplyBase(), m_memory.ReplyBytes(), m_memory.ReplySlotCount());
        replies.Post(seq, status, payload, size);
        return MOBILEGL_OK;
    }
    MobileGLResult ShmLink::ReadReply(std::uint64_t seq, std::int32_t* status, const void** payload,
                                      std::uint64_t* size) {
        if (!status || !payload || !size) return MOBILEGL_ERR_INVALID_ARGUMENT;
        ReplySlotPool replies(m_memory.ReplyBase(), m_memory.ReplyBytes(), m_memory.ReplySlotCount());
        return replies.ReadView(seq, status, payload, size) ? MOBILEGL_OK : MOBILEGL_ERR_PROTOCOL_MISMATCH;
    }
    std::uint64_t ShmLink::EventPublishedHead() const {
        return m_memory.Valid() ? m_memory.EventControl()->cmdHead.load(std::memory_order_acquire) : 0;
    }
    MobileGLResult ShmLink::WaitForEventDelivery(std::uint64_t head, std::uint32_t) {
        return EventPublishedHead() >= head ? MOBILEGL_OK : MOBILEGL_ERR_PROTOCOL_MISMATCH;
    }
    Doorbell& ShmLink::ConsumerBell() {
        return *m_consumer;
    }
    Doorbell& ShmLink::ProducerBell() {
        return *m_producer;
    }
    void ShmLink::Detach() {
        m_commandsOut = {};
        m_commandsIn = {};
        m_eventsOut = {};
        m_eventsIn = {};
        m_memory.Close();
    }
} // namespace MobileGL::MG_Remote::Transport
