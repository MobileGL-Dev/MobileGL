// MobileGL - MobileGL/MG_Remote/Transport/EventRing.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// SEG_EVENT: the server -> client reverse channel. Owner: package s1.
//
// WHAT P5 OWES HERE AND NOTHING MORE (BRIEF R-12, and s1's brief): it must be
// able to CARRY OnBufferWriteback, OnGpuWritten and OnSurfaceChanged. The other
// seven MGPipeCallbacks members are off P5's reduced path, and THE OVERFLOW
// POLICY IS P9's - what is here is the mechanism (a full ring latches
// eventRingFull, a lossy post that is dropped counts in eventDropped) and not a
// policy that decides between them.
//
// IT IS A SECOND RingControl, NOT A THIRD CURSOR SET. RingControl carries two
// cursor triples (SEG_CMD and SEG_STAGE) and adding a third would resize the
// shared page that Ring.h static_asserts at exactly 4096 bytes. So SEG_EVENT
// gets its OWN control page at its own head and drives it with the Cmd cursor
// set - the same RingProducer/RingConsumer code, in the opposite direction. The
// two EVENT FLAGS still live in the SEG_CMD page, because that is where Ring.h
// declares them and where the client's own waits already look.
//
// THE PAYLOAD SHAPES ARE FIXED-WIDTH AND LIVE HERE, not in MG_Pipe's headers.
// Nothing under Transport/ may reach MobileGL/Includes.h (WireLog.h:9-24, and
// the purity gate's `wire-header` probe), so MGPipeHandle / MGPRange /
// MGPSurfaceInfo cannot be named in this file. The wire shapes below mirror them
// field for field and the SESSIONS assert the two agree, which is the same
// discipline the rest of the wire is held to: a wire struct is fixed-width, and
// the conversion happens where the frontend types are legal.

#pragma once

#include "Doorbell.h"
#include "Ring.h"

#include <cstdint>
#include <cstring>

namespace MobileGL::MG_Remote::Transport {

    // Record kinds on SEG_EVENT. 0 is kRingPadRecordKind and can never be an
    // event, which is why the list starts at 1.
    enum EventKind : std::uint16_t {
        kEventNone = 0,
        kEventBufferWriteback = 1, // MGPipeCallbacks::OnBufferWriteback
        kEventGpuWritten = 2,      // MGPipeCallbacks::OnGpuWritten
        kEventSurfaceChanged = 3,  // MGPipeCallbacks::OnSurfaceChanged
    };

    // The 8-byte {slot, gen} pair, mirrored (MGPipeHandles.h:54-65).
    struct EventHandle {
        std::uint32_t Slot;
        std::uint32_t Gen;
    };
    static_assert(sizeof(EventHandle) == 8, "the handle is the 8-byte {slot, gen} pair");

    // MGPRange, mirrored (MGPipeTypes.h:63-67).
    struct EventRange {
        std::uint64_t Offset;
        std::uint64_t Size;
    };
    static_assert(sizeof(EventRange) == 16, "MGPRange is 16 bytes on the wire");

    // OnBufferWriteback(res, offset, MGPBlobRef bytes). The bytes follow this
    // head INSIDE THE RECORD: the blobref the client hands the frontend names
    // SEG_EVENT and the in-segment offset of those inline bytes, which is what
    // makes "the destination is the client's shadow" (contract table 1 row 22)
    // reachable without a second segment. `Size` is therefore the record's own
    // tail length and is cross-checked against it.
    struct EventBufferWritebackHead {
        EventHandle Resource;
        std::uint64_t Offset; // destination offset inside the resource
        std::uint64_t Size;   // inline byte count that follows
    };
    static_assert(sizeof(EventBufferWritebackHead) == 24, "wire shape");

    // OnGpuWritten(res, rangeCount, ranges). EventRange[RangeCount] follows.
    struct EventGpuWrittenHead {
        EventHandle Resource;
        std::uint32_t RangeCount;
        std::uint32_t Pad0;
    };
    static_assert(sizeof(EventGpuWrittenHead) == 16, "wire shape");

    // OnSurfaceChanged(const MGPSurfaceInfo*), mirrored (MGPipeTypes.h:1394-1401).
    struct EventSurfaceChangedHead {
        std::uint32_t Width;
        std::uint32_t Height;
        std::uint32_t InternalFormat;
        std::uint16_t Samples;
        std::uint16_t Layers;
        std::uint8_t IsDefault;
        std::uint8_t Pad0[7];
    };
    static_assert(sizeof(EventSurfaceChangedHead) == 24, "MGPSurfaceInfo is 24 bytes on the wire");

    // The server's end. One producer: the apply thread, by construction.
    class EventRingProducer {
    public:
        EventRingProducer() = default;

        // `eventControl` is SEG_EVENT's own control page; `cmdControl` is the
        // SEG_CMD page, because Ring.h declares eventRingFull / eventDropped
        // there and the client's waits already look at it.
        EventRingProducer(RingControl* eventControl, RingControl* cmdControl, void* base,
                          std::uint64_t capacityBytes)
            : m_cmdControl(cmdControl),
              m_producer(eventControl, base, capacityBytes, RingCursorSet::Cmd) {}

        bool Valid() const { return m_producer.Valid() && m_cmdControl != nullptr; }

        // Reserves one event record. nullptr means the ring is full: the caller
        // decides, and the two flags are how it says which decision it took.
        // THIS FUNCTION DOES NOT DECIDE - that is P9's.
        void* Reserve(EventKind kind, std::uint64_t payloadBytes) {
            void* slot = m_producer.Reserve(static_cast<std::uint16_t>(kind), kRecNone, payloadBytes);
            if (slot == nullptr && m_cmdControl != nullptr) {
                // "SEG_EVENT full, server stopped applying" - Ring.h:123. Latched
                // here, cleared by the consumer once it has drained.
                m_cmdControl->eventRingFull.store(1, std::memory_order_release);
            }
            return slot;
        }

        // For a LOSSY event the caller could not place. Lossless events must
        // never call this; they wait for the client to drain instead.
        void CountDrop() {
            if (m_cmdControl != nullptr) {
                m_cmdControl->eventDropped.fetch_add(1, std::memory_order_relaxed);
            }
        }

        // Publish, THEN ring - the same order as the forward direction, and for
        // the same reason (Doorbell.h:186-193: the fence only orders what
        // precedes it, so ringing first reopens the lost-wakeup window).
        void PublishAndNotify(Doorbell& clientBell, std::atomic<std::uint32_t>& producerParked) {
            m_producer.Publish();
            NotifyIfParked(clientBell, producerParked);
        }

        RingProducer& Ring() { return m_producer; }

    private:
        RingControl* m_cmdControl = nullptr;
        RingProducer m_producer;
    };

    // The client's end. One consumer: the GL thread, which drains between verbs.
    class EventRingConsumer {
    public:
        EventRingConsumer() = default;

        EventRingConsumer(RingControl* eventControl, RingControl* cmdControl, void* base,
                          std::uint64_t capacityBytes, const void* segmentBase)
            : m_cmdControl(cmdControl),
              m_consumer(eventControl, base, capacityBytes, RingCursorSet::Cmd),
              m_segmentBase(static_cast<const std::uint8_t*>(segmentBase)) {}

        bool Valid() const { return m_consumer.Valid() && m_cmdControl != nullptr; }

        bool Pop(RingRecordView& out, bool* outCorrupt = nullptr) {
            return m_consumer.Pop(out, outCorrupt);
        }

        // Release the bytes and clear the full latch. Only after the caller has
        // finished with every payload pointer it popped: a writeback's bytes live
        // in the ring itself, so retiring early is the R-11 violation one level
        // down.
        void Drained() {
            m_consumer.PublishRetired();
            if (m_cmdControl != nullptr) {
                m_cmdControl->eventRingFull.store(0, std::memory_order_release);
            }
        }

        // Byte offset of `payload` inside SEG_EVENT, which is what an
        // OnBufferWriteback MGPBlobRef must carry (Seg = kSegEvent, Offset =
        // this, Size = the head's Size). Never a host address - R-2's rule B.
        std::uint64_t OffsetInSegment(const void* payload) const {
            return static_cast<std::uint64_t>(static_cast<const std::uint8_t*>(payload) -
                                              m_segmentBase);
        }

        std::uint64_t DroppedEvents() const {
            return m_cmdControl == nullptr
                       ? 0
                       : m_cmdControl->eventDropped.load(std::memory_order_relaxed);
        }
        bool RingIsFull() const {
            return m_cmdControl != nullptr &&
                   m_cmdControl->eventRingFull.load(std::memory_order_acquire) != 0;
        }

        RingConsumer& Ring() { return m_consumer; }

    private:
        RingControl* m_cmdControl = nullptr;
        RingConsumer m_consumer;
        const std::uint8_t* m_segmentBase = nullptr;
    };

} // namespace MobileGL::MG_Remote::Transport
