// SPDX-License-Identifier: LGPL-3.0-only
#include "StreamLink.h"
#include "SessionRings.h"
#include "ShmLink.h"
#include "Doorbell.h"
#include "ReplySlot.h"
#include "Framing.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#if !defined(_WIN32)
#include <cerrno>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#if defined(__linux__)
#include <pthread.h>
#endif
#endif

namespace MobileGL::MG_Remote::Transport {
    namespace {
        constexpr std::uint64_t kDataEnvelopeBytes = 20;
        constexpr std::uint64_t kMaxDataPayload = kMaxFramePayloadSize - kDataEnvelopeBytes;
        constexpr std::uint32_t kDataMagic = 0x444c474d; // MGLD, little endian
        enum Kind : unsigned {
            Cmd = 1,
            Stage,
            ClientProgress,
            ProgressFrame,
            Reply,
            Event
        };
        void Put32(std::uint8_t* p, std::uint32_t v) {
            for (unsigned i = 0; i < 4; ++i)
                p[i] = v >> (i * 8);
        }
        void Put64(std::uint8_t* p, std::uint64_t v) {
            for (unsigned i = 0; i < 8; ++i)
                p[i] = v >> (i * 8);
        }
        std::uint32_t Get32(const std::uint8_t* p) {
            std::uint32_t v = 0;
            for (unsigned i = 0; i < 4; ++i)
                v |= std::uint32_t(p[i]) << (i * 8);
            return v;
        }
        std::uint64_t Get64(const std::uint8_t* p) {
            std::uint64_t v = 0;
            for (unsigned i = 0; i < 8; ++i)
                v |= std::uint64_t(p[i]) << (i * 8);
            return v;
        }
        [[noreturn]] void Corrupt(const char* detail) {
            WireLogFatal("MGPipe: Fatal{ProtocolCorruption, \"StreamLink\"} %s", detail);
        }
        void Advance(std::atomic<std::uint64_t>& value, std::uint64_t next) {
            if (next < value.load(std::memory_order_acquire)) Corrupt("watermark moved backwards");
            value.store(next, std::memory_order_release);
        }
        class StreamBell final : public Doorbell {
        public:
            void Notify() override { m_bell.Notify(); }
            bool Park(std::uint32_t timeout) override { return m_bell.Park(timeout); }
            void Reset() override { m_bell.Reset(); }
            bool Dead() const override { return m_bell.Dead(); }
            bool PeerHungUp() const override { return m_peer.load(std::memory_order_acquire); }
            void Kill(bool peer) {
                if (peer) m_peer.store(true, std::memory_order_release);
                m_bell.Kill();
            }

        private:
            CondVarDoorbell m_bell;
            std::atomic<bool> m_peer{false};
        };
    } // namespace

    struct StreamLink::Impl {
        std::unique_ptr<ShmLink> owned = std::make_unique<ShmLink>();
        SessionSegments* memory = nullptr;
        TransportRoleTag role = TransportRoleTag::ClientProducer;
        int fd = -1;
        std::atomic<bool> closed{false}, stopping{false};
        StreamBell consumer, producer;
        std::thread reader;
        std::mutex writeMutex, replyMutex;
        std::condition_variable replyReady;
        std::vector<LinkSpan> stage;
        std::vector<std::uint8_t> reply, readReply;
        std::uint64_t replySeq = 0, sentCmd = 0, receivedCmd = 0, sentEvent = 0, receivedEvent = 0;
        std::int32_t replyStatus = 0;
        std::uint64_t lastProgressSeq = 0;
        std::chrono::steady_clock::time_point lastProgress = std::chrono::steady_clock::now();
        LinkArena records{}, events{};
        LinkCursor recordCursor{}, eventCursor{};
        bool Client() const { return role == TransportRoleTag::ClientProducer; }
        RingControl& C() const { return *memory->CmdControl(); }
        RingControl& E() const { return *memory->EventControl(); }
        std::uint64_t MaxReply() const {
            return memory->ReplyBytes() / memory->ReplySlotCount() - sizeof(ReplySlotHeader);
        }
        void Die(bool peer) {
            {
                // Pair every reply-wait predicate transition with its mutex:
                // an EOF between the predicate and cv.wait must not lose a wake.
                std::lock_guard<std::mutex> lock(replyMutex);
                closed.store(true, std::memory_order_release);
            }
            consumer.Kill(peer);
            producer.Kill(peer);
            replyReady.notify_all();
#if !defined(_WIN32)
            if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
#endif
        }
        bool Read(void* data, std::size_t size) {
#if !defined(_WIN32)
            auto* p = static_cast<std::uint8_t*>(data);
            while (size) {
                const auto n = ::recv(fd, p, size, 0);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) {
                    Die(!stopping.load(std::memory_order_acquire));
                    return false;
                }
                p += n;
                size -= static_cast<std::size_t>(n);
            }
            return true;
#else
            (void)data;
            (void)size;
            return false;
#endif
        }
        bool Send(unsigned kind, std::uint64_t a, std::uint64_t b, const void* payload, std::uint64_t size) {
            if (size > kMaxDataPayload) Corrupt("outgoing frame exceeds limit");
            std::uint8_t h[28]{};
            Put32(h, kDataMagic);
            Put32(h + 4, static_cast<std::uint32_t>(kDataEnvelopeBytes + size));
            h[8] = static_cast<std::uint8_t>(kind);
            Put64(h + 12, a);
            Put64(h + 20, b);
#if !defined(_WIN32)
            // One gather write keeps a small command and its envelope in one
            // TCP send (TCP_NODELAY is enabled). Partial writes retain the same
            // frame lock and advance through both vectors without copying.
            iovec vectors[2] = {{h, sizeof h}, {const_cast<void*>(payload), static_cast<std::size_t>(size)}};
            msghdr message{};
            message.msg_iov = vectors;
            message.msg_iovlen = size ? 2 : 1;
            while (message.msg_iovlen) {
                const auto sent = ::sendmsg(fd, &message, MSG_NOSIGNAL);
                if (sent < 0 && errno == EINTR) continue;
                if (sent <= 0) {
                    Die(!stopping.load(std::memory_order_acquire));
                    return false;
                }
                auto consumed = static_cast<std::size_t>(sent);
                while (message.msg_iovlen && consumed >= message.msg_iov->iov_len) {
                    consumed -= message.msg_iov->iov_len;
                    ++message.msg_iov;
                    --message.msg_iovlen;
                }
                if (message.msg_iovlen && consumed) {
                    message.msg_iov->iov_base = static_cast<std::uint8_t*>(message.msg_iov->iov_base) + consumed;
                    message.msg_iov->iov_len -= consumed;
                }
            }
            return true;
#else
            (void)payload;
            return false;
#endif
        }
        bool SendRing(unsigned kind, void* base, std::uint64_t capacity, std::uint64_t head, std::uint64_t& sent) {
            if (head < sent || head - sent > capacity) Corrupt("outgoing ring window invalid");
            while (sent < head) {
                const auto at = sent % capacity;
                const auto count = std::min({head - sent, capacity - at, kMaxDataPayload});
                if (!Send(kind, sent, 0, static_cast<std::uint8_t*>(base) + at, count)) return false;
                sent += count;
            }
            return true;
        }
        bool SendEvents() {
            return SendRing(Event, memory->EventRingBase(), memory->EventRingCapacity(),
                            E().cmdHead.load(std::memory_order_acquire), sentEvent);
        }
        bool SendProgress() {
            // Events and replies always precede the applied watermark that publishes them.
            if (!SendEvents()) return false;
            std::uint8_t p[64]{};
            Put64(p, C().Progress.appliedSeq.load(std::memory_order_acquire));
            Put64(p + 8, C().Progress.retiredSeq.load(std::memory_order_acquire));
            Put64(p + 16, C().Progress.completedFrameSerial.load(std::memory_order_acquire));
            Put64(p + 24, C().Progress.presentAckSerial.load(std::memory_order_acquire));
            Put64(p + 32, C().cmdAppliedTail.load(std::memory_order_acquire));
            // FreeBytes uses RETIRED tail. Sending only applied tail silently permits
            // reuse of borrowed command slots, and omitting this tail wedges on wrap.
            Put64(p + 40, C().cmdRetiredTail.load(std::memory_order_acquire));
            Put64(p + 48, C().eventRingFull.load(std::memory_order_acquire));
            Put64(p + 56, C().eventDropped.load(std::memory_order_acquire));
            lastProgressSeq = Get64(p);
            lastProgress = std::chrono::steady_clock::now();
            return Send(ProgressFrame, 0, 0, p, sizeof p);
        }
        void ReceiveRing(void* base, std::uint64_t capacity, std::uint64_t tail, std::uint64_t a,
                         const std::uint8_t* payload, std::uint64_t size, std::uint64_t& received) {
            if (a != received || a < tail || a - tail > capacity || size > capacity - (a - tail) ||
                size > capacity - a % capacity || size == 0)
                Corrupt("ring frame outside receive window");
            std::memcpy(static_cast<std::uint8_t*>(base) + a % capacity, payload, static_cast<std::size_t>(size));
            received += size;
        }
        void Loop() {
#if defined(__linux__)
            pthread_setname_np(pthread_self(), Client() ? "mgl-cli-io" : "mgl-srv-io");
#endif
            FrameReader frames(kDataMagic);
            for (;;) {
                std::uint8_t h[28];
                if (!Read(h, kFrameHeaderSize)) return;
                if (frames.Feed(h, kFrameHeaderSize) != MOBILEGL_OK) Corrupt("invalid data frame header");
                const auto framedSize = Get32(h + 4);
                if (framedSize < kDataEnvelopeBytes) Corrupt("truncated data envelope");
                if (!Read(h + kFrameHeaderSize, kDataEnvelopeBytes)) return;
                if (frames.Feed(h + kFrameHeaderSize, kDataEnvelopeBytes) != MOBILEGL_OK)
                    Corrupt("invalid data frame envelope");
                const auto size = framedSize - static_cast<std::uint32_t>(kDataEnvelopeBytes);
                const auto kind = unsigned(h[8]);
                const auto a = Get64(h + 12), b = Get64(h + 20);
                if (Get32(h) != kDataMagic || h[9] || h[10] || h[11] || size > kMaxDataPayload)
                    Corrupt("invalid data frame header");
                // Reject the size/window before allocating or reading a hostile payload.
                if ((kind == Stage && (Client() || a > memory->StageBytes() || size > memory->StageBytes() - a)) ||
                    (kind == Cmd && (Client() || size > memory->CmdRingCapacity())) ||
                    (kind == Event && (!Client() || size > memory->EventRingCapacity())) ||
                    (kind == Reply && (!Client() || size > MaxReply())) ||
                    (kind == ProgressFrame && (!Client() || size != 64)) ||
                    (kind == ClientProgress && (Client() || size != 8)) || kind < Cmd || kind > Event)
                    Corrupt("frame kind or size outside negotiated window");
                if (((kind == Cmd || kind == Stage || kind == Event) && b != 0) ||
                    (kind == ProgressFrame && (a != 0 || b != 0)) || (kind == Reply && b > UINT32_MAX))
                    Corrupt("nonzero reserved data envelope field");
                std::uint8_t chunk[64 * 1024];
                std::uint64_t remaining = size;
                while (remaining) {
                    const auto count = std::min<std::uint64_t>(remaining, sizeof chunk);
                    if (!Read(chunk, static_cast<std::size_t>(count))) return;
                    if (frames.Feed(chunk, count) != MOBILEGL_OK) Corrupt("invalid data frame payload");
                    remaining -= count;
                }
                std::vector<std::uint8_t> p;
                if (frames.TakeMessage(p) != MOBILEGL_OK) Corrupt("incomplete data frame");
                const auto* payload = p.data() + kDataEnvelopeBytes;
                if (kind == Stage) {
                    std::memcpy(static_cast<std::uint8_t*>(memory->StageBase()) + a, payload, size);
                } else if (kind == Cmd) {
                    ReceiveRing(memory->CmdRingBase(), memory->CmdRingCapacity(),
                                C().cmdRetiredTail.load(std::memory_order_acquire), a, payload, size, receivedCmd);
                } else if (kind == ClientProgress) {
                    if (a != receivedCmd) Corrupt("cmdHead does not match delivered bytes");
                    const auto tail = Get64(payload);
                    if (tail > E().cmdHead.load(std::memory_order_acquire)) Corrupt("event tail ahead of head");
                    const auto oldTail = E().cmdRetiredTail.load(std::memory_order_acquire);
                    Advance(E().cmdAppliedTail, tail);
                    Advance(E().cmdRetiredTail, tail);
                    // Only a real drain can clear a latched backlog. A command-only
                    // publication carrying the old tail must not undo a new full latch.
                    if (tail > oldTail) C().eventRingFull.store(0, std::memory_order_release);
                    Watermark::AdvanceSubmitted(C(), b);
                    Advance(C().cmdHead, a);
                    consumer.Notify();
                } else if (kind == Event) {
                    ReceiveRing(memory->EventRingBase(), memory->EventRingCapacity(),
                                E().cmdRetiredTail.load(std::memory_order_acquire), a, payload, size, receivedEvent);
                    Advance(E().cmdHead, receivedEvent);
                    producer.Notify();
                } else if (kind == ProgressFrame) {
                    const auto* v = payload;
                    if (Get64(v + 8) > Get64(v) || Get64(v) > C().submittedSeq.load(std::memory_order_acquire) ||
                        Get64(v + 40) > Get64(v + 32) || Get64(v + 32) > C().cmdHead.load(std::memory_order_acquire) ||
                        Get64(v + 48) > 1)
                        Corrupt("progress ahead of delivered work");
                    Advance(C().cmdAppliedTail, Get64(v + 32));
                    Advance(C().cmdRetiredTail, Get64(v + 40));
                    C().eventRingFull.store(static_cast<std::uint32_t>(Get64(v + 48)), std::memory_order_release);
                    C().eventDropped.store(static_cast<std::uint32_t>(Get64(v + 56)), std::memory_order_release);
                    Watermark::AdvanceCompletedFrame(C(), Get64(v + 16));
                    Watermark::AdvancePresentAck(C(), Get64(v + 24));
                    {
                        std::lock_guard<std::mutex> lock(replyMutex);
                        Watermark::AdvanceApplied(C(), Get64(v));
                        // AdvanceRetired clamps to local appliedSeq. Apply this
                        // received snapshot in dependency order or the final
                        // retirement is silently clamped to the previous batch.
                        Watermark::AdvanceRetired(C(), Get64(v + 8));
                    }
                    replyReady.notify_all();
                    producer.Notify();
                } else if (kind == Reply) {
                    {
                        std::lock_guard<std::mutex> lock(replyMutex);
                        if (a <= replySeq) Corrupt("reply sequence moved backwards");
                        replySeq = a;
                        replyStatus = static_cast<std::int32_t>(static_cast<std::uint32_t>(b));
                        reply.assign(payload, payload + size);
                    }
                    replyReady.notify_all();
                    producer.Notify();
                }
            }
        }
    };

    StreamLink::StreamLink() : m_impl(new Impl) {}
    StreamLink::~StreamLink() {
        Detach();
    }
    MobileGLResult StreamLink::Attach(int fd, SessionSegments& mirrors, TransportRoleTag role) {
#if defined(_WIN32)
        (void)fd;
        (void)mirrors;
        (void)role;
        return MOBILEGL_ERR_UNSUPPORTED;
#else
        if (fd < 0 || !mirrors.Valid() || !mirrors.ReplySlotCount() ||
            mirrors.ReplyBytes() / mirrors.ReplySlotCount() <= sizeof(ReplySlotHeader))
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        Detach();
        m_impl.reset(new Impl);
        auto& x = *m_impl;
        x.fd = fd;
        x.memory = &mirrors;
        x.role = role;
        x.records = {mirrors.CmdRingBase(), mirrors.CmdRingCapacity(), mirrors.CmdRingCapacity() - 1, 0};
        x.recordCursor = {mirrors.CmdRingBase(), mirrors.CmdRingCapacity(), mirrors.CmdRingCapacity() - 1, 0};
        x.events = {mirrors.EventRingBase(), mirrors.EventRingCapacity(), mirrors.EventRingCapacity() - 1, 0};
        x.eventCursor = {mirrors.EventRingBase(), mirrors.EventRingCapacity(), mirrors.EventRingCapacity() - 1, 0};
        x.reader = std::thread([&x] { x.Loop(); });
        return MOBILEGL_OK;
#endif
    }
    MobileGLResult StreamLink::AttachOwned(int fd, const SessionSegmentSizes& sizes, TransportRoleTag role) {
        auto owned = std::make_unique<ShmLink>();
        const auto result = owned->Memory().CreatePrivate(
            sizes, role == TransportRoleTag::ClientProducer ? MemoryRole::Client : MemoryRole::Server);
        if (result != MOBILEGL_OK) return result;
        const auto attached = Attach(fd, owned->Memory(), role);
        if (attached == MOBILEGL_OK) m_impl->owned = std::move(owned);
        return attached;
    }
    MobileGLResult CreateStreamLink(int fd, const SessionSegmentSizes& sizes, TransportRoleTag role,
                                    std::unique_ptr<ILink>& out) {
        auto link = std::make_unique<StreamLink>();
        const auto result = link->AttachOwned(fd, sizes, role);
        if (result == MOBILEGL_OK) out = std::move(link);
#if !defined(_WIN32)
        else if (fd >= 0)
            ::close(fd);
#endif
        return result;
    }
    void StreamLink::InitializeEndpoints() {
        m_impl->owned->SetRole(m_impl->role);
        m_impl->owned->InitializeEndpoints();
    }
    SessionSegments& StreamLink::Memory() {
        return m_impl->memory ? *m_impl->memory : m_impl->owned->Memory();
    }
    RingProducer& StreamLink::CommandsOut() {
        return m_impl->owned->CommandsOut();
    }
    RingConsumer& StreamLink::CommandsIn() {
        return m_impl->owned->CommandsIn();
    }
    EventRingProducer& StreamLink::EventsOut() {
        return m_impl->owned->EventsOut();
    }
    EventRingConsumer& StreamLink::EventsIn() {
        return m_impl->owned->EventsIn();
    }
    LinkCapabilities StreamLink::Capabilities() const {
        if (!m_impl->memory) return {};
        return {false, false, false, m_impl->memory->CmdRingCapacity() / 2, m_impl->MaxReply()};
    }
    LinkProgress* StreamLink::Progress() {
        return m_impl->memory ? &m_impl->C().Progress : nullptr;
    }
    LinkEventFlags* StreamLink::EventFlags() {
        return m_impl->memory ? reinterpret_cast<LinkEventFlags*>(&m_impl->C().eventRingFull) : nullptr;
    }
    LinkArena* StreamLink::RecordArena() {
        return m_impl->Client() ? &m_impl->records : nullptr;
    }
    LinkCursor* StreamLink::RecordCursor() {
        return m_impl->Client() ? nullptr : &m_impl->recordCursor;
    }
    LinkArena* StreamLink::EventArena() {
        return m_impl->Client() ? nullptr : &m_impl->events;
    }
    LinkCursor* StreamLink::EventCursor() {
        return m_impl->Client() ? &m_impl->eventCursor : nullptr;
    }
    Doorbell& StreamLink::ConsumerBell() {
        return m_impl->consumer;
    }
    Doorbell& StreamLink::ProducerBell() {
        return m_impl->producer;
    }
    MobileGLResult StreamLink::StageBytes(std::uint64_t, LinkSpan*, void**) {
        return MOBILEGL_ERR_UNSUPPORTED;
    }
    MobileGLResult StreamLink::ResolveSpan(LinkSegment segment, LinkSpan span, const void** out) {
        if (!out || !m_impl->memory) return MOBILEGL_ERR_INVALID_ARGUMENT;
        auto& m = *m_impl->memory;
        const void* base = nullptr;
        std::uint64_t cap = 0;
        switch (segment) {
        case LinkSegment::Cmd:
            base = m.CmdRingBase();
            cap = m.CmdRingCapacity();
            break;
        case LinkSegment::Stage:
            base = m.StageBase();
            cap = m.StageBytes();
            break;
        case LinkSegment::Reply:
            return MOBILEGL_ERR_UNSUPPORTED;
        case LinkSegment::Event:
            base = m.EventSegmentBase();
            cap = m.AnnouncedSize(SessionSegmentSlot::Event);
            break;
        default:
            return MOBILEGL_ERR_UNSUPPORTED;
        }
        if (span.Offset > cap || span.Size > cap - span.Offset) return MOBILEGL_ERR_PROTOCOL_MISMATCH;
        *out = static_cast<const std::uint8_t*>(base) + span.Offset;
        return MOBILEGL_OK;
    }
    void StreamLink::NoteStage(LinkSpan span) {
        auto& x = *m_impl;
        if (!x.Client() || span.Offset > x.memory->StageBytes() || span.Size > x.memory->StageBytes() - span.Offset)
            Corrupt("staged span outside window");
        if (!x.stage.empty() && x.stage.back().Offset + x.stage.back().Size == span.Offset)
            x.stage.back().Size += span.Size;
        else
            x.stage.push_back(span);
    }
    MobileGLResult StreamLink::Flush() {
        auto& x = *m_impl;
        if (!Attached()) return MOBILEGL_ERR_TRANSPORT_CLOSED;
        std::lock_guard<std::mutex> lock(x.writeMutex);
        if (!x.Client()) return x.SendEvents() ? MOBILEGL_OK : MOBILEGL_ERR_TRANSPORT_CLOSED;
        for (const auto span : x.stage) {
            std::uint64_t sent = 0;
            while (sent < span.Size) {
                const auto count = std::min(span.Size - sent, kMaxDataPayload);
                if (!x.Send(Stage, span.Offset + sent, 0,
                            static_cast<std::uint8_t*>(x.memory->StageBase()) + span.Offset + sent, count))
                    return MOBILEGL_ERR_TRANSPORT_CLOSED;
                sent += count;
            }
        }
        x.stage.clear();
        const auto head = x.C().cmdHead.load(std::memory_order_acquire);
        if (!x.SendRing(Cmd, x.memory->CmdRingBase(), x.memory->CmdRingCapacity(), head, x.sentCmd))
            return MOBILEGL_ERR_TRANSPORT_CLOSED;
        std::uint8_t tail[8];
        Put64(tail, x.E().cmdRetiredTail.load(std::memory_order_acquire));
        return x.Send(ClientProgress, head, x.C().submittedSeq.load(std::memory_order_acquire), tail, sizeof tail)
                   ? MOBILEGL_OK
                   : MOBILEGL_ERR_TRANSPORT_CLOSED;
    }
    MobileGLResult StreamLink::FlushProgress() {
        auto& x = *m_impl;
        if (!Attached()) return MOBILEGL_ERR_TRANSPORT_CLOSED;
        if (x.Client()) return Flush();
        std::lock_guard<std::mutex> lock(x.writeMutex);
        return x.SendProgress() ? MOBILEGL_OK : MOBILEGL_ERR_TRANSPORT_CLOSED;
    }
    void StreamLink::ProgressChanged() {
        auto& x = *m_impl;
        if (x.Client() || !Attached()) return;
        const auto seq = x.C().Progress.appliedSeq.load(std::memory_order_acquire);
        if (seq - x.lastProgressSeq >= 64 ||
            std::chrono::steady_clock::now() - x.lastProgress >= std::chrono::milliseconds(1))
            FlushProgress();
    }
    MobileGLResult StreamLink::PostReply(std::uint64_t seq, std::int32_t status, const void* p, std::uint64_t size) {
        auto& x = *m_impl;
        if (x.Client() || !Attached()) return MOBILEGL_ERR_TRANSPORT_CLOSED;
        if (size > x.MaxReply() || (size && !p)) return MOBILEGL_ERR_BUFFER_TOO_SMALL;
        std::lock_guard<std::mutex> lock(x.writeMutex);
        return x.Send(Reply, seq, static_cast<std::uint32_t>(status), p, size) && x.SendProgress()
                   ? MOBILEGL_OK
                   : MOBILEGL_ERR_TRANSPORT_CLOSED;
    }
    MobileGLResult StreamLink::ReadReply(std::uint64_t seq, std::int32_t* status, const void** p, std::uint64_t* size) {
        auto& x = *m_impl;
        if (!status || !p || !size || !seq || !x.Client()) return MOBILEGL_ERR_INVALID_ARGUMENT;
        if (!x.memory) return MOBILEGL_ERR_NOT_INITIALIZED;
        std::unique_lock<std::mutex> lock(x.replyMutex);
        // The applied watermark is sent after the reply, so normal callers never
        // wait here. A standalone reply reader may wait without polling.
        x.replyReady.wait(lock, [&] {
            return x.closed.load(std::memory_order_acquire) || x.replySeq >= seq ||
                   x.C().Progress.appliedSeq.load(std::memory_order_acquire) >= seq;
        });
        if (x.replySeq != seq) return x.closed.load() ? MOBILEGL_ERR_TRANSPORT_CLOSED : MOBILEGL_ERR_PROTOCOL_MISMATCH;
        x.readReply = x.reply;
        *status = x.replyStatus;
        *size = x.readReply.size();
        *p = x.readReply.data();
        return MOBILEGL_OK;
    }
    std::uint64_t StreamLink::EventPublishedHead() const {
        return m_impl->memory ? m_impl->E().cmdHead.load(std::memory_order_acquire) : 0;
    }
    MobileGLResult StreamLink::WaitForEventDelivery(std::uint64_t head, std::uint32_t timeoutMs) {
        auto& x = *m_impl;
        if (!x.memory) return MOBILEGL_ERR_NOT_INITIALIZED;
        const auto ready = [&] { return x.E().cmdHead.load(std::memory_order_acquire) >= head; };
        if (ready()) return MOBILEGL_OK;
        const bool delivered = x.producer.Wait(x.C().producerParked, ready, 0, timeoutMs);
        return delivered ? MOBILEGL_OK : x.producer.Dead() ? MOBILEGL_ERR_TRANSPORT_CLOSED : MOBILEGL_ERR_TIMEOUT;
    }
    bool StreamLink::Attached() const {
        return m_impl->memory && !m_impl->closed.load(std::memory_order_acquire);
    }
    bool StreamLink::PeerHungUp() const {
        return m_impl->producer.PeerHungUp();
    }
    TransportRoleTag StreamLink::Role() const {
        return m_impl->role;
    }
    void StreamLink::Detach() {
        auto& x = *m_impl;
        if (!x.memory) return;
        x.stopping.store(true, std::memory_order_release);
        x.Die(false);
        if (x.reader.joinable()) x.reader.join();
#if !defined(_WIN32)
        if (x.fd >= 0) ::close(x.fd);
#endif
        x.fd = -1;
        x.memory = nullptr;
    }
} // namespace MobileGL::MG_Remote::Transport
