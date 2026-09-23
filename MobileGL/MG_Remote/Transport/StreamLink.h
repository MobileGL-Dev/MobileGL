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
        // PH-7 (4), ID-P7-3. The SERVER's half of a nonce-bound data connection, in two steps.
        // The owned memory has to exist before the connection does - Welcome announces its
        // sizes, and Welcome is what carries the nonce the client's data connection presents -
        // so the link is attached WITHOUT a descriptor and without its reader thread, and
        // BindDataFd starts both once the connection has proved which session it belongs to.
        // Until then every write answers TRANSPORT_CLOSED (there is no fd) rather than blocking.
        MobileGLResult AttachOwnedDeferred(const struct SessionSegmentSizes& sizes, TransportRoleTag role);
        MobileGLResult BindDataFd(int dataFd);
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
        MobileGLResult Prepare(SessionSegments& mirrors, TransportRoleTag role);
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
} // namespace MobileGL::MG_Remote::Transport
