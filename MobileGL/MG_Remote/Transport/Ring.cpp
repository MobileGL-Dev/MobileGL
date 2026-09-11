// MobileGL - MobileGL/MG_Remote/Transport/Ring.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "Ring.h"

#include "SessionRings.h"

#include <MG_Util/Debug/Log.h>

#include <cstring>

namespace MobileGL::MG_Remote::Transport {

    namespace {
        constexpr std::uint64_t Align8(std::uint64_t value) {
            return (value + (kRingRecordAlignment - 1)) & ~(kRingRecordAlignment - 1);
        }

        bool IsPowerOfTwo(std::uint64_t value) { return value != 0 && (value & (value - 1)) == 0; }

        std::atomic<std::uint64_t>& Head(RingControl& c, RingCursorSet which) {
            return which == RingCursorSet::Cmd ? c.cmdHead : c.stageHead;
        }
        const std::atomic<std::uint64_t>& Head(const RingControl& c, RingCursorSet which) {
            return which == RingCursorSet::Cmd ? c.cmdHead : c.stageHead;
        }
        std::atomic<std::uint64_t>& AppliedTail(RingControl& c, RingCursorSet which) {
            return which == RingCursorSet::Cmd ? c.cmdAppliedTail : c.stageAppliedTail;
        }
        const std::atomic<std::uint64_t>& AppliedTail(const RingControl& c, RingCursorSet which) {
            return which == RingCursorSet::Cmd ? c.cmdAppliedTail : c.stageAppliedTail;
        }
        std::atomic<std::uint64_t>& RetiredTail(RingControl& c, RingCursorSet which) {
            return which == RingCursorSet::Cmd ? c.cmdRetiredTail : c.stageRetiredTail;
        }
        const std::atomic<std::uint64_t>& RetiredTail(const RingControl& c, RingCursorSet which) {
            return which == RingCursorSet::Cmd ? c.cmdRetiredTail : c.stageRetiredTail;
        }
    } // namespace

    void InitRingControl(RingControl& control) {
        std::memset(static_cast<void*>(&control), 0, sizeof(RingControl));
        // 0 means "uninitialized" for both generations, so a peer that reads a
        // zero page can tell it from a legal generation.
        control.serverEpoch.store(1, std::memory_order_relaxed);
        control.ringGeneration.store(1, std::memory_order_relaxed);
    }

    bool RingCursorsValid(const RingControl& control, RingCursorSet cursors,
                          std::uint64_t capacityBytes) {
        const std::uint64_t head = Head(control, cursors).load(std::memory_order_acquire);
        const std::uint64_t applied = AppliedTail(control, cursors).load(std::memory_order_acquire);
        const std::uint64_t retired = RetiredTail(control, cursors).load(std::memory_order_acquire);
        if (applied > head || retired > applied) {
            return false;
        }
        return head - retired <= capacityBytes;
    }

    MobileGLResult HardDrainRing(RingControl& control, RingCursorSet cursors) {
        const std::uint64_t head = Head(control, cursors).load(std::memory_order_acquire);
        const std::uint64_t applied = AppliedTail(control, cursors).load(std::memory_order_acquire);
        const std::uint64_t retired = RetiredTail(control, cursors).load(std::memory_order_acquire);
        if (head != applied || applied != retired) {
            MGLOG_E("MG_Remote ring: hard drain refused, ring is not quiesced "
                    "(head=%llu applied=%llu retired=%llu)",
                    static_cast<unsigned long long>(head),
                    static_cast<unsigned long long>(applied),
                    static_cast<unsigned long long>(retired));
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        // Cursors stay monotonic across the drain - only the generation moves,
        // so any offset either side cached is now recognisably stale.
        control.ringGeneration.fetch_add(1, std::memory_order_acq_rel);
        return MOBILEGL_OK;
    }

    // -----------------------------------------------------------------------
    // Producer
    // -----------------------------------------------------------------------

    RingProducer::RingProducer(RingControl* control, void* base, std::uint64_t capacityBytes,
                               RingCursorSet cursors)
        : m_control(control), m_base(static_cast<std::uint8_t*>(base)), m_capacity(capacityBytes),
          m_mask(capacityBytes - 1), m_cursors(cursors) {
        if (control == nullptr || base == nullptr || !IsPowerOfTwo(capacityBytes) ||
            capacityBytes < kMinRingCapacity || capacityBytes > kMaxRingCapacity) {
            MGLOG_E("MG_Remote ring: producer rejected, capacity %llu must be a power of two "
                    "between %llu and %llu bytes over a non-null mapping (a record may be at most "
                    "half the ring, and the record header's size field is 32-bit, so a bigger ring "
                    "would truncate it)",
                    static_cast<unsigned long long>(capacityBytes),
                    static_cast<unsigned long long>(kMinRingCapacity),
                    static_cast<unsigned long long>(kMaxRingCapacity));
            m_control = nullptr;
            m_base = nullptr;
            m_capacity = 0;
            m_mask = 0;
            return;
        }
        m_localHead = Head(*control, cursors).load(std::memory_order_acquire);
    }

    std::uint64_t RingProducer::TailForReclaim() const {
        // The conservative watermark: a slot borrowed into the GPU timeline is
        // only free after retiredTail passes it. A consumer that never borrows
        // publishes retired together with applied, so this costs nothing there.
        return RetiredTail(*m_control, m_cursors).load(std::memory_order_acquire);
    }

    std::uint64_t RingProducer::FreeBytes() const {
        if (m_control == nullptr) {
            return 0;
        }
        const std::uint64_t inFlight = m_localHead - TailForReclaim();
        return inFlight >= m_capacity ? 0 : m_capacity - inFlight;
    }

    void* RingProducer::Reserve(std::uint16_t kind, std::uint16_t flags,
                                std::uint64_t payloadBytes) {
        if (m_control == nullptr) {
            return nullptr;
        }
        const std::uint64_t total = Align8(sizeof(RingRecordHeader) + payloadBytes);
        if (total > MaxRecordBytes()) {
            // A single record larger than HALF the ring is a caller bug: the
            // record catalogue has to chunk oversized payloads (large subdata
            // becomes several records) rather than emit one giant record.
            //
            // Half, not the whole ring, because a record has to be placeable at
            // EVERY head offset of an empty ring. Straddling the wrap boundary
            // costs a pad of spaceToEnd bytes on top of the record, and with
            // spaceToEnd < total that is at most 2*total-8, which stays within
            // the capacity exactly up to capacity/2. Above it the record is
            // placeable at some offsets and not at others: at head offset 16 of
            // an empty 256-byte ring a 248-byte record needs 240+248 bytes while
            // FreeBytes() reports 256, so a producer that waits for FreeBytes()
            // >= total stalls forever, and nothing is ever logged. Refusing here
            // makes that impossible - a nullptr with FreeBytes() >= total can no
            // longer mean "wait".
            MGLOG_E("MG_Remote ring: record kind %u of %llu bytes exceeds half of a %llu byte ring; "
                    "the emitter must chunk it",
                    static_cast<unsigned>(kind), static_cast<unsigned long long>(total),
                    static_cast<unsigned long long>(m_capacity));
            return nullptr;
        }

        const std::uint64_t offset = m_localHead & m_mask;
        const std::uint64_t spaceToEnd = m_capacity - offset;
        // Every record is a multiple of 8, so the distance to the wrap boundary
        // is too, and a pad header always fits.
        const bool needsPad = spaceToEnd < total;
        const std::uint64_t needed = needsPad ? spaceToEnd + total : total;
        if (FreeBytes() < needed) {
            MGLOG_D("MG_Remote ring: full, %llu bytes free, %llu needed",
                    static_cast<unsigned long long>(FreeBytes()),
                    static_cast<unsigned long long>(needed));
            return nullptr;
        }

        if (needsPad) {
            RingRecordHeader pad{};
            pad.kind = kRingPadRecordKind;
            pad.flags = kRecPad;
            pad.size = static_cast<std::uint32_t>(spaceToEnd);
            std::memcpy(SlotAt(m_localHead), &pad, sizeof(pad));
            m_localHead += spaceToEnd;
        }

        RingRecordHeader header{};
        header.kind = kind;
        header.flags = static_cast<std::uint16_t>(flags & ~static_cast<std::uint16_t>(kRecPad));
        header.size = static_cast<std::uint32_t>(total);
        std::uint8_t* slot = SlotAt(m_localHead);
        std::memcpy(slot, &header, sizeof(header));
        m_localHead += total;
        return slot + sizeof(RingRecordHeader);
    }

    void RingProducer::Publish() {
        if (m_control == nullptr) {
            return;
        }
        // Release: everything written into the slots happens-before the peer's
        // acquire load of the head.
        Head(*m_control, m_cursors).store(m_localHead, std::memory_order_release);
    }

    // -----------------------------------------------------------------------
    // Consumer
    // -----------------------------------------------------------------------

    RingConsumer::RingConsumer(RingControl* control, void* base, std::uint64_t capacityBytes,
                               RingCursorSet cursors)
        : m_control(control), m_base(static_cast<const std::uint8_t*>(base)),
          m_capacity(capacityBytes), m_mask(capacityBytes - 1), m_cursors(cursors) {
        if (control == nullptr || base == nullptr || !IsPowerOfTwo(capacityBytes) ||
            capacityBytes < kMinRingCapacity || capacityBytes > kMaxRingCapacity) {
            MGLOG_E("MG_Remote ring: consumer rejected, capacity %llu must be a power of two "
                    "between %llu and %llu bytes over a non-null mapping (a record may be at most "
                    "half the ring, and the record header's size field is 32-bit, so a bigger ring "
                    "would truncate it)",
                    static_cast<unsigned long long>(capacityBytes),
                    static_cast<unsigned long long>(kMinRingCapacity),
                    static_cast<unsigned long long>(kMaxRingCapacity));
            m_control = nullptr;
            m_base = nullptr;
            m_capacity = 0;
            m_mask = 0;
            return;
        }
        m_localTail = AppliedTail(*control, cursors).load(std::memory_order_acquire);
    }

    bool RingConsumer::Pop(RingRecordView& out, bool* outCorrupt) {
        if (outCorrupt != nullptr) {
            *outCorrupt = false;
        }
        if (m_control == nullptr) {
            return false;
        }
        const std::uint64_t head = Head(*m_control, m_cursors).load(std::memory_order_acquire);
        while (m_localTail != head) {
            const std::uint64_t available = head - m_localTail;
            if (available < sizeof(RingRecordHeader) || available > m_capacity) {
                MGLOG_E("MG_Remote ring: %llu bytes between tail and head is impossible for a %llu "
                        "byte ring",
                        static_cast<unsigned long long>(available),
                        static_cast<unsigned long long>(m_capacity));
                if (outCorrupt != nullptr) {
                    *outCorrupt = true;
                }
                return false;
            }
            const std::uint64_t offset = m_localTail & m_mask;
            RingRecordHeader header{};
            std::memcpy(&header, m_base + offset, sizeof(header));

            // SEG_CMD is written by the peer process: compile-time asserts on
            // record sizes cannot see runtime corruption, so every dispatch is
            // preceded by these bounds checks and a violation is fatal, never a
            // retry (plan section 6.3, runtime bounds discipline).
            const std::uint64_t size = header.size;
            if (size < sizeof(RingRecordHeader) || (size % kRingRecordAlignment) != 0 ||
                size > available || offset + size > m_capacity) {
                MGLOG_E("MG_Remote ring: corrupt record header at cursor %llu "
                        "(kind=%u flags=0x%04X size=%u available=%llu)",
                        static_cast<unsigned long long>(m_localTail),
                        static_cast<unsigned>(header.kind), static_cast<unsigned>(header.flags),
                        header.size, static_cast<unsigned long long>(available));
                if (outCorrupt != nullptr) {
                    *outCorrupt = true;
                }
                return false;
            }

            if ((header.flags & kRecPad) != 0) {
                m_localTail += size;
                continue;
            }

            out.kind = header.kind;
            out.flags = header.flags;
            out.payload = m_base + offset + sizeof(RingRecordHeader);
            // Includes the alignment tail; the record catalogue knows the real
            // payload length.
            out.payloadSize = size - sizeof(RingRecordHeader);
            out.cursor = m_localTail;
            m_localTail += size;
            return true;
        }
        return false;
    }

    void RingConsumer::PublishApplied() {
        if (m_control == nullptr) {
            return;
        }
        AppliedTail(*m_control, m_cursors).store(m_localTail, std::memory_order_release);
    }

    void RingConsumer::PublishRetired() {
        if (m_control == nullptr) {
            return;
        }
        // retiredTail must never overtake appliedTail, so publish both.
        AppliedTail(*m_control, m_cursors).store(m_localTail, std::memory_order_release);
        RetiredTail(*m_control, m_cursors).store(m_localTail, std::memory_order_release);
    }

    void RingConsumer::PublishRetiredUpTo(std::uint64_t cursor) {
        if (m_control == nullptr) {
            return;
        }
        const std::uint64_t applied = AppliedTail(*m_control, m_cursors).load(std::memory_order_acquire);
        const std::uint64_t clamped = cursor > applied ? applied : cursor;
        const std::uint64_t current = RetiredTail(*m_control, m_cursors).load(std::memory_order_relaxed);
        if (clamped > current) {
            RetiredTail(*m_control, m_cursors).store(clamped, std::memory_order_release);
        }
    }

    // =======================================================================
    // P5: the five watermarks, the two session endpoints, and the ABI mixer.
    // =======================================================================

    std::uint64_t LargestPowerOfTwoAtMost(std::uint64_t bytes) {
        if (bytes == 0) {
            return 0;
        }
        std::uint64_t value = 1;
        while (value <= (bytes >> 1)) {
            value <<= 1;
        }
        return value;
    }

    std::uint64_t RingCapacityForSegment(std::uint64_t segmentBytes) {
        if (segmentBytes <= sizeof(RingControl)) {
            return 0;
        }
        const std::uint64_t usable = LargestPowerOfTwoAtMost(segmentBytes - sizeof(RingControl));
        return usable < kMinRingCapacity ? 0 : usable;
    }

    namespace Watermark {

        namespace {
            // A watermark may be published LATE but never EARLY, and it may never
            // move BACKWARDS. Backwards is the half that is mechanically
            // detectable from inside, so it is refused loudly here; "early" can
            // only be caught at the call site, which is why every advance below
            // has exactly one caller and a named unit case.
            void AdvanceMonotonic(std::atomic<std::uint64_t>& watermark, std::uint64_t to,
                                  const char* name) {
                const std::uint64_t current = watermark.load(std::memory_order_relaxed);
                if (to < current) {
                    MGLOG_E("MG_Remote watermark: refusing to move %s backwards, %llu -> %llu; a "
                            "waiter that already resumed on the higher value cannot be un-resumed",
                            name, static_cast<unsigned long long>(current),
                            static_cast<unsigned long long>(to));
                    return;
                }
                if (to == current) {
                    return;
                }
                // Release: everything the advance is a statement ABOUT - the
                // record that was applied, the staged bytes that were drained,
                // the reply that was posted - must be visible to the acquiring
                // waiter before the number that says it happened.
                watermark.store(to, std::memory_order_release);
            }
        } // namespace

        void AdvanceSubmitted(RingControl& control, std::uint64_t seq) {
            AdvanceMonotonic(control.submittedSeq, seq, "submittedSeq");
        }

        void AdvanceApplied(RingControl& control, std::uint64_t seq) {
            AdvanceMonotonic(control.appliedSeq, seq, "appliedSeq");
        }

        void AdvanceRetired(RingControl& control, std::uint64_t seq) {
            // retiredSeq may never overtake appliedSeq: the staging allocator
            // reclaims behind it, so a retire ahead of the apply hands live bytes
            // back to the producer. Clamped rather than refused, because a
            // caller that retires "everything applied" is the normal shape.
            const std::uint64_t applied = control.appliedSeq.load(std::memory_order_acquire);
            AdvanceMonotonic(control.retiredSeq, seq > applied ? applied : seq, "retiredSeq");
        }

        void AdvanceCompletedFrame(RingControl& control, std::uint64_t serial) {
            AdvanceMonotonic(control.completedFrameSerial, serial, "completedFrameSerial");
        }

        void AdvancePresentAck(RingControl& control, std::uint64_t serial) {
            AdvanceMonotonic(control.presentAckSerial, serial, "presentAckSerial");
        }

    } // namespace Watermark

    // -----------------------------------------------------------------------
    // SessionProducer
    // -----------------------------------------------------------------------

    void SessionProducer::Attach(RingControl* control, RingProducer* cmd, RingProducer* stage,
                                 Doorbell* peerBell, Doorbell* selfBell, std::uint32_t spinUs) {
        m_control = control;
        m_cmd = cmd;
        m_stage = stage;
        m_peerBell = peerBell;
        m_selfBell = selfBell;
        m_spinUs = spinUs;
    }

    void SessionProducer::Detach() {
        m_control = nullptr;
        m_cmd = nullptr;
        m_stage = nullptr;
        m_peerBell = nullptr;
        m_selfBell = nullptr;
    }

    void SessionProducer::PublishAndNotify(std::uint64_t submittedSeq) {
        if (!Valid()) {
            return;
        }
        // 1. the records themselves.
        m_cmd->Publish();
        if (m_stage != nullptr) {
            m_stage->Publish();
        }
        // 2. the diagnostic watermark, after the bytes it describes.
        Watermark::AdvanceSubmitted(*m_control, submittedSeq);
        // 3. and only now the bell. Publish-then-ring, never ring-then-publish.
        if (m_peerBell != nullptr) {
            NotifyIfParked(*m_peerBell, m_control->consumerParked);
        }
    }

    template <class Ready>
    SessionWait SessionProducer::Park(Ready&& ready, std::uint32_t timeoutMs) {
        if (!Valid() || m_selfBell == nullptr) {
            return SessionWait::TimedOut;
        }
        if (m_selfBell->Wait(m_control->producerParked, ready, m_spinUs, timeoutMs)) {
            return SessionWait::Reached;
        }
        // Wait == false && Dead() is "the session was shut down", and it is the
        // only thing that returns from a kWaitForever park. Anything else is the
        // deadline.
        return m_selfBell->Dead() ? SessionWait::ShutDown : SessionWait::TimedOut;
    }

    SessionWait SessionProducer::WaitForApplied(std::uint64_t seq, std::uint32_t timeoutMs) {
        if (!Valid()) {
            return SessionWait::TimedOut;
        }
        RingControl* control = m_control;
        return Park([control, seq] { return Watermark::Reached(control->appliedSeq, seq); },
                    timeoutMs);
    }

    SessionWait SessionProducer::WaitForPresentAck(std::uint64_t serial, std::uint32_t timeoutMs) {
        if (!Valid()) {
            return SessionWait::TimedOut;
        }
        RingControl* control = m_control;
        return Park([control, serial] { return Watermark::Reached(control->presentAckSerial, serial); },
                    timeoutMs);
    }

    SessionWait SessionProducer::WaitForCmdSpace(std::uint64_t bytes, std::uint32_t timeoutMs) {
        if (!Valid()) {
            return SessionWait::TimedOut;
        }
        RingProducer* cmd = m_cmd;
        return Park([cmd, bytes] { return cmd->FreeBytes() >= bytes; }, timeoutMs);
    }

    SessionWait SessionProducer::WaitForStageSpace(std::uint64_t bytes, std::uint32_t timeoutMs) {
        if (!Valid() || m_stage == nullptr) {
            return SessionWait::TimedOut;
        }
        RingProducer* stage = m_stage;
        return Park([stage, bytes] { return stage->FreeBytes() >= bytes; }, timeoutMs);
    }

    // -----------------------------------------------------------------------
    // SessionConsumer
    // -----------------------------------------------------------------------

    void SessionConsumer::Attach(RingControl* control, RingConsumer* cmd, Doorbell* peerBell,
                                 Doorbell* selfBell, std::uint32_t spinUs) {
        m_control = control;
        m_cmd = cmd;
        m_peerBell = peerBell;
        m_selfBell = selfBell;
        m_spinUs = spinUs;
        m_appliedSeq = control == nullptr ? 0 : control->appliedSeq.load(std::memory_order_acquire);
    }

    void SessionConsumer::Detach() {
        m_control = nullptr;
        m_cmd = nullptr;
        m_peerBell = nullptr;
        m_selfBell = nullptr;
    }

    SessionWait SessionConsumer::WaitForWork(std::uint32_t timeoutMs) {
        if (!Valid() || m_selfBell == nullptr) {
            return SessionWait::TimedOut;
        }
        RingControl* control = m_control;
        RingConsumer* cmd = m_cmd;
        const bool woke = m_selfBell->Wait(
            control->consumerParked,
            [control, cmd] {
                return control->cmdHead.load(std::memory_order_acquire) != cmd->LocalTail();
            },
            m_spinUs, timeoutMs);
        if (woke) {
            return SessionWait::Reached;
        }
        return m_selfBell->Dead() ? SessionWait::ShutDown : SessionWait::TimedOut;
    }

    void SessionConsumer::RetireThrough(std::uint64_t seq) {
        if (!Valid()) {
            return;
        }
        Watermark::AdvanceRetired(*m_control, seq);
        m_cmd->PublishRetired();
        NotifyClient();
    }

    void SessionConsumer::NotifyClient() {
        if (m_control != nullptr && m_peerBell != nullptr) {
            NotifyIfParked(*m_peerBell, m_control->producerParked);
        }
    }

    // -----------------------------------------------------------------------
    // The ABI fingerprint's mixer
    // -----------------------------------------------------------------------

    std::uint64_t MixAbiFingerprint(std::uint64_t dynamicParamsSize, std::uint64_t capsSize,
                                    std::uint64_t functionTableSize, std::uint32_t abiVersion,
                                    const char* buildStamp) {
        // FNV-1a over the four numbers and the stamp. Not a hash with any
        // security property and not meant to be one: it has to (a) change when
        // ANY input changes and (b) be computable identically in two processes
        // built from one source tree, which rules out anything seeded at runtime.
        std::uint64_t hash = 1469598103934665603ull;
        const auto mix = [&hash](std::uint64_t value) {
            for (int byte = 0; byte < 8; ++byte) {
                hash ^= static_cast<std::uint64_t>((value >> (byte * 8)) & 0xFF);
                hash *= 1099511628211ull;
            }
        };
        mix(dynamicParamsSize);
        mix(capsSize);
        mix(functionTableSize);
        mix(abiVersion);
        if (buildStamp != nullptr) {
            for (const char* c = buildStamp; *c != '\0'; ++c) {
                hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(*c));
                hash *= 1099511628211ull;
            }
        }
        // 0 is reserved for "not stated": a peer that forgot to fill the field
        // must not accidentally agree with one that did.
        return hash == 0 ? 1ull : hash;
    }

} // namespace MobileGL::MG_Remote::Transport
