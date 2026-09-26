// SPDX-License-Identifier: LGPL-3.0-only
#include "StreamLink.h"
#include "SessionRings.h"
#include "ShmLink.h"
#include "Doorbell.h"
#include "ReplySlot.h"
#include "Framing.h"
// P12: the declared-answer store's crossing log. Same include as LinkMetrics.cpp's, rather than
// <Includes.h>, because this file is transport-layer and pulls nothing else from the tree.
#include <MG_Util/Debug/Log.h>
#include <algorithm>
#include <array>
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
    // P65Read's counters: written by the server's io thread, drained by its apply thread (see
    // StreamLink::TakeReadStats).
    static std::atomic<std::uint64_t> gReadBytes{0}, gReadCalls{0}, gReadReads{0};
    static std::atomic<std::uint64_t> gReadNsInRecv{0}, gReadNsTotal{0};
    // P65CLIENTPACE's send half: the io thread's side of the same question. A sendmsg that blocks
    // is the socket full, i.e. the WRITE side's back-pressure - and on a client whose frame is 80%
    // waiting, "is it waiting to hand bytes over, or waiting for the game to make them" is the
    // whole question. Time spent inside sendmsg, not calls made.
    static std::atomic<std::uint64_t> gSendBytes{0}, gSendCalls{0}, gSendNsInSend{0};
    static std::atomic<std::uint64_t> gSendNsTotal{0};
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
        std::vector<std::uint8_t> readReply;
        std::uint64_t replySeq = 0, sentCmd = 0, receivedCmd = 0, sentEvent = 0, receivedEvent = 0;
        // ---- THE RECENT-REPLY RING (P12) -------------------------------------------------
        //
        // ILink::ReadReply takes a SEQ, and until P12 that was decoration: every caller read the
        // answer it had just waited for, which is always the newest one, so a single slot answered
        // every question and `replySeq != seq` could stand in for "no such answer". The create
        // window is the first caller that reads an OLDER seq - it defers up to four answers and
        // takes them in order - and against a single slot every one of those reads fails
        // PROTOCOL_MISMATCH for ever. What that looked like on the device, measured: the window
        // filled to 4 and STAYED there, `refusals=0` because no answer was ever read, every one of
        // the frame's 4,536 creates fell through to the blocking path, and the wall clock did not
        // move (29.0 s against the 30.1 s of the same binary with the window off). The deferral
        // was pure overhead: four answers held unread, plus one failed drain attempt per record.
        //
        // ---- AND THE RING WAS NOT ENOUGH, SO THE STORE IS KEYED ON A DECLARATION (P12) -------
        //
        // THE MEASUREMENT THAT SETTLED IT. With the ring at 8 the drain read back NOTHING:
        // `taken=63 pushed=65 readfail=8,872` on a load frame, and the miss probe showed why -
        // `want=310` on every miss while the ring had moved on to [1175..1504]. Between two
        // creates this client emits on the order of a thousand records and looks at the window
        // not once, so a deferral has to survive an unbounded amount of other traffic, and WHAT
        // DISPLACES IT IS NOT WHAT THE CLIENT EMITS BUT WHAT THE SERVER ANSWERS - it is behind,
        // and its late answers to the EARLIER creates fill a bounded buffer first.
        //
        // Ring 8 -> 1024 as a one-line experiment, and every deferred answer came back:
        // `taken=4,499/4,500 readfail=0`, wall 27.9 s -> 23.0 s, and with the residency budget
        // out of the way 28.66 s -> 16.80 s at window 2 and 11.45 s at window 4. So the property
        // needed is "the answer is still there when the reader comes back", and 1024 is a
        // STAND-IN for it, not the design: 1024 retained answers is ~2 GiB worst case, and "deep
        // enough" is a probability rather than a bound.
        //
        // THE BOUND COMES FROM THE CLIENT. A seq is retained only if the wire DECLARED it will be
        // read (ILink::DeclareReplyRead), so the outstanding set is the window's depth plus the
        // one blocking row in flight plus whatever a retired window strands - it does not depend
        // on how many answers the server chooses to send. Retaining on ARRIVAL instead would grow
        // ~11.3 entries per create against a server that ignores the no-reply bit (55,903 answers
        // a frame where this client reads 4,501), and any cap would then be reached and would
        // evict precisely the oldest WANTED answer - the same wedge, a few hundred creates later.
        //
        // OVERFLOW IS FATAL AND NEVER AN EVICTION. An evicted wanted answer is unrecoverable:
        // sending another create to re-latch the object wipes the server's record for it
        // (PipeApply.cpp:2033) along with its PendingUploads (PipeApply.h:332), and the client
        // cleared its dirty flags at EMISSION (PipeApply.h:298-304), so nobody re-sends those
        // texels. The capacity is sized so that cannot be reached, and reaching it is a bug that
        // says its own name.
        // ONE DEFINITION, IN THE HEADER, because the client's window is bounded by this number.
        static constexpr std::size_t kRetainedCap = StreamLink::kRetainedAnswersMax;
        struct StoredReply {
            std::uint64_t seq = 0;
            std::int32_t status = 0;
            std::vector<std::uint8_t> bytes;
        };
        // MONOTONE ON BOTH SIDES, so a cursor beats a hash map: the wire declares in emission
        // order, the server answers in apply order. It also makes a declared-but-never-answered
        // seq visible for free - the cursor walks past it on the way to a later answer - which is
        // counted rather than ignored, because the read that follows will fail and should fail
        // with a number beside it.
        std::vector<std::uint64_t> wanted;
        std::size_t wantCursor = 0;
        std::vector<StoredReply> retained;
        std::uint64_t skippedUnwanted = 0, unansweredWanted = 0, retainedTotal = 0;
        // P12: the most declared answers this store ever held at once. The cap is an overflow
        // Fatal rather than an eviction, so the MAXIMUM is the number that says whether a given
        // window depth has room - a sample at teardown answers a different question.
        std::uint64_t retainedPeak = 0;
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
            // P65Read: THE SERVER'S SOCKET-READ PATH, WHICH NOTHING MEASURED. The per-frame
            // clocks say the frame's time is NOT in the client's GL thread (13%) and NOT in
            // PipeApplier::ApplyOne (11%) - 76% of a 105-185 ms frame is outside both, and the
            // only stage between them is this loop. Two different worlds look identical from
            // outside it and need opposite fixes:
            //   * few BIG recvs with little time blocked -> the reader is copying, i.e. one
            //     thread's throughput is the ceiling;
            //   * many SMALL recvs, most of the wall spent blocked -> the reader is STARVED and
            //     the client (or the batching on its side) sets the pace.
            // Counted here rather than sampled from /proc because the question is per-call, not
            // per-thread. Printed every kReadMarkBytes so a frame's worth of traffic yields
            // several lines and no line can be lost to a quiet period.
            constexpr std::uint64_t kReadMarkBytes = 8ull * 1024ull * 1024ull;
            (void)kReadMarkBytes;
            const auto readStarted = std::chrono::steady_clock::now();
            std::uint64_t calls = 0;
            auto* p = static_cast<std::uint8_t*>(data);
            const auto wanted = size;
            while (size) {
                const auto recvStarted = std::chrono::steady_clock::now();
                const auto n = ::recv(fd, p, size, 0);
                gReadNsInRecv.fetch_add(static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - recvStarted).count()),
                    std::memory_order_relaxed);
                ++calls;
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) {
                    Die(!stopping.load(std::memory_order_acquire));
                    return false;
                }
                p += n;
                size -= static_cast<std::size_t>(n);
            }
            gReadCalls.fetch_add(calls, std::memory_order_relaxed);
            gReadReads.fetch_add(1, std::memory_order_relaxed);
            gReadBytes.fetch_add(wanted, std::memory_order_relaxed);
            gReadNsTotal.fetch_add(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - readStarted).count()),
                std::memory_order_relaxed);
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
            const auto sendStarted = std::chrono::steady_clock::now();
            std::uint64_t sendCalls = 0;
            while (message.msg_iovlen) {
                const auto syscallStarted = std::chrono::steady_clock::now();
                const auto sent = ::sendmsg(fd, &message, MSG_NOSIGNAL);
                gSendNsInSend.fetch_add(static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - syscallStarted).count()),
                    std::memory_order_relaxed);
                ++sendCalls;
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
            gSendCalls.fetch_add(sendCalls, std::memory_order_relaxed);
            gSendBytes.fetch_add(size, std::memory_order_relaxed);
            gSendNsTotal.fetch_add(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - sendStarted).count()),
                std::memory_order_relaxed);
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
                        // DECLARED? The cursor walks past any DECLARED seq this answer has
                        // overtaken: the server answers in apply order, so an answer with a
                        // higher seq means those will never come. That is a broken invariant
                        // rather than a race, and it is counted - the read that follows fails
                        // and should fail with this number beside it.
                        while (wantCursor < wanted.size() && wanted[wantCursor] < a) {
                            ++wantCursor;
                            ++unansweredWanted;
                        }
                        if (wantCursor < wanted.size() && wanted[wantCursor] == a) {
                            ++wantCursor;
                            // OVERFLOW IS NAMED, NOT ABSORBED. See the capacity note above:
                            // an eviction here would throw away an answer somebody is waiting
                            // for, and there is no path that recovers one.
                            if (retained.size() >= kRetainedCap) {
                                Corrupt("the declared-answer store overflowed; a declared answer"
                                        " would have to be evicted and none is recoverable");
                            }
                            StoredReply& slot = retained.emplace_back();
                            slot.seq = a;
                            slot.status = static_cast<std::int32_t>(static_cast<std::uint32_t>(b));
                            slot.bytes.assign(payload, payload + size);
                            ++retainedTotal;
                            // LOGGED AS IT CROSSES EVERY QUARTER OF THE CAP, not sampled: at most
                            // four lines per run, and they are the ones a "how deep may the window
                            // go" question is answered from without a rebuild (P12's
                            // MOBILEGL_IPC_CREATE_WINDOW sweep reads exactly these).
                            if (retained.size() > retainedPeak) {
                                retainedPeak = retained.size();
                                if (retainedPeak % (kRetainedCap / 4) == 0) {
                                    MGLOG_I("P12 store: peak=%llu of cap=%llu declared answers "
                                            "outstanding; the window's depth is what is spent here",
                                            static_cast<unsigned long long>(retainedPeak),
                                            static_cast<unsigned long long>(kRetainedCap));
                                }
                            }
                        } else {
                            // NOBODY DECLARED IT: drop it. This is what keeps the store bounded
                            // against a server that answers records this client never reads.
                            ++skippedUnwanted;
                        }
                    }
                    replyReady.notify_all();
                    producer.Notify();
                }
            }
        }
    };

    StreamLink::ReadStats StreamLink::TakeSendStats() {
        ReadStats out;
        out.bytes = gSendBytes.exchange(0, std::memory_order_relaxed);
        out.calls = gSendCalls.exchange(0, std::memory_order_relaxed);
        out.nsInRecv = gSendNsInSend.exchange(0, std::memory_order_relaxed);
        out.nsTotal = gSendNsTotal.exchange(0, std::memory_order_relaxed);
        return out;
    }

    StreamLink::ReadStats StreamLink::TakeReadStats() {
        ReadStats out;
        out.bytes = gReadBytes.exchange(0, std::memory_order_relaxed);
        out.calls = gReadCalls.exchange(0, std::memory_order_relaxed);
        out.reads = gReadReads.exchange(0, std::memory_order_relaxed);
        out.nsInRecv = gReadNsInRecv.exchange(0, std::memory_order_relaxed);
        out.nsTotal = gReadNsTotal.exchange(0, std::memory_order_relaxed);
        return out;
    }

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
        if (fd < 0) return MOBILEGL_ERR_INVALID_ARGUMENT;
        const auto prepared = Prepare(mirrors, role);
        if (prepared != MOBILEGL_OK) return prepared;
        return BindDataFd(fd);
#endif
    }
    MobileGLResult StreamLink::Prepare(SessionSegments& mirrors, TransportRoleTag role) {
        if (!mirrors.Valid() || !mirrors.ReplySlotCount() ||
            mirrors.ReplyBytes() / mirrors.ReplySlotCount() <= sizeof(ReplySlotHeader))
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        Detach();
        m_impl.reset(new Impl);
        auto& x = *m_impl;
        x.memory = &mirrors;
        x.role = role;
        x.records = {mirrors.CmdRingBase(), mirrors.CmdRingCapacity(), mirrors.CmdRingCapacity() - 1, 0};
        x.recordCursor = {mirrors.CmdRingBase(), mirrors.CmdRingCapacity(), mirrors.CmdRingCapacity() - 1, 0};
        x.events = {mirrors.EventRingBase(), mirrors.EventRingCapacity(), mirrors.EventRingCapacity() - 1, 0};
        x.eventCursor = {mirrors.EventRingBase(), mirrors.EventRingCapacity(), mirrors.EventRingCapacity() - 1, 0};
        return MOBILEGL_OK;
    }
    MobileGLResult StreamLink::BindDataFd(int fd) {
#if defined(_WIN32)
        (void)fd;
        return MOBILEGL_ERR_UNSUPPORTED;
#else
        auto& x = *m_impl;
        // Once, onto a prepared link: a second descriptor would be a second reader on one ring.
        if (fd < 0 || !x.memory || x.fd >= 0 || x.reader.joinable()) return MOBILEGL_ERR_INVALID_ARGUMENT;
        x.fd = fd;
        x.reader = std::thread([&x] { x.Loop(); });
        return MOBILEGL_OK;
#endif
    }
    MobileGLResult StreamLink::AttachOwnedDeferred(const SessionSegmentSizes& sizes, TransportRoleTag role) {
#if defined(_WIN32)
        (void)sizes;
        (void)role;
        return MOBILEGL_ERR_UNSUPPORTED;
#else
        auto owned = std::make_unique<ShmLink>();
        const auto result = owned->Memory().CreatePrivate(
            sizes, role == TransportRoleTag::ClientProducer ? MemoryRole::Client : MemoryRole::Server);
        if (result != MOBILEGL_OK) return result;
        const auto prepared = Prepare(owned->Memory(), role);
        if (prepared == MOBILEGL_OK) m_impl->owned = std::move(owned);
        return prepared;
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
    MobileGLResult CreateDeferredStreamLink(const SessionSegmentSizes& sizes, TransportRoleTag role,
                                            std::unique_ptr<ILink>& out) {
        auto link = std::make_unique<StreamLink>();
        const auto result = link->AttachOwnedDeferred(sizes, role);
        if (result == MOBILEGL_OK) out = std::move(link);
        return result;
    }
    MobileGLResult BindStreamLinkDataFd(ILink& link, int fd) {
        auto* stream = dynamic_cast<StreamLink*>(&link);
        if (stream == nullptr) return MOBILEGL_ERR_INVALID_ARGUMENT;
        return stream->BindDataFd(fd);
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
        return {false, false, false, m_impl->memory->CmdRingCapacity() / 2, m_impl->MaxReply(),
                /*RetainsReplies=*/true};
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
        // BY SEQ, WHICH IS WHAT THE PARAMETER HAS ALWAYS SAID (P12). The equality this replaced
        // was true only while every caller read the newest answer; it is what made a deferred
        // answer unreadable and the create window inert on this transport.
        for (auto it = x.retained.begin(); it != x.retained.end(); ++it) {
            if (it->seq != seq) continue;
            // TAKE-AND-DELETE IS ONE STEP UNDER THE LOCK. The bytes are SWAPPED into the shared
            // scratch before the entry goes, so whoever is handed the answer is whoever consumed
            // it - no copy of a large answer, and no window in which a second reader could see
            // the same seq. (The pointer outlives the lock, as it always has here; this link has
            // ONE reply reader, and the store's erase is what keeps that an invariant rather
            // than a hope.)
            x.readReply.swap(it->bytes);
            *status = it->status;
            *size = x.readReply.size();
            *p = x.readReply.data();
            x.retained.erase(it);
            return MOBILEGL_OK;
        }
        return x.closed.load() ? MOBILEGL_ERR_TRANSPORT_CLOSED : MOBILEGL_ERR_PROTOCOL_MISMATCH;
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
    void StreamLink::DeclareReplyRead(std::uint64_t seq) {
        auto& x = *m_impl;
        if (!x.Client() || !x.memory) return;
        std::lock_guard<std::mutex> lock(x.replyMutex);
        // MONOTONE, IN EMISSION ORDER (the wire declares before it publishes, and seqs only ever
        // grow). An out-of-order or repeated declaration is dropped rather than stored: the first
        // would break the cursor, and the second is a retry that must not grow the list.
        if (!x.wanted.empty() && seq <= x.wanted.back()) return;
        x.wanted.push_back(seq);
        // AND THE CONSUMED PREFIX IS RELEASED, or the list would grow by one entry per declared
        // record for the life of the session (4,536 a frame). The cursor's entries can never be
        // needed again: a seq is answered once.
        if (x.wantCursor >= 64) {
            x.wanted.erase(x.wanted.begin(),
                           x.wanted.begin() + static_cast<std::ptrdiff_t>(x.wantCursor));
            x.wantCursor = 0;
        }
    }

    std::uint64_t StreamLink::RetainedOutstanding() const {
        auto& x = *m_impl;
        std::lock_guard<std::mutex> lock(x.replyMutex);
        return static_cast<std::uint64_t>(x.retained.size());
    }
    std::uint64_t StreamLink::RetainedTotal() const { return m_impl->retainedTotal; }
    std::uint64_t StreamLink::SkippedUnwanted() const { return m_impl->skippedUnwanted; }
    std::uint64_t StreamLink::UnansweredWanted() const { return m_impl->unansweredWanted; }

    void StreamLink::Detach() {
        auto& x = *m_impl;
        if (!x.memory) return;
        x.stopping.store(true, std::memory_order_release);
        x.Die(false);
        if (x.reader.joinable()) x.reader.join();
        // P12: the declared-answer store does not outlive the link. Without this a detached
        // session leaves up to a frame's worth of answers (and their seqs) resident.
        {
            std::lock_guard<std::mutex> lock(x.replyMutex);
            x.wanted.clear();
            x.wantCursor = 0;
            x.retained.clear();
        }
#if !defined(_WIN32)
        if (x.fd >= 0) ::close(x.fd);
#endif
        x.fd = -1;
        x.memory = nullptr;
    }
} // namespace MobileGL::MG_Remote::Transport
