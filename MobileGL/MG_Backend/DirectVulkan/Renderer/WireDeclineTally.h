// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/WireDeclineTally.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include <Config.h>
#if MOBILEGL_BUILD_DISAGGREGATED

#include <Includes.h>
#include <MG_Util/Debug/Log.h>

#include <atomic>

namespace MobileGL::MG_Backend::DirectVulkan {

// P7 wave 2 package B3: the per-site tally behind CONTRACT-P7 §0 rule I.
//
// Shape is PipeApplier.h's ServerVerbSink tallies, and for the same reason (R-16: a probe may
// not arm against a stub). "The retrace passed" and "the retrace silently dropped nine draws"
// produce the same exit code and the same log, so the lane has to assert the number that only
// this path can move.
//
// Counters are process-wide and monotone. On the spawn arm they live in the SERVER process,
// which is where the draw path runs - a client-side reader would always see zero, so the
// two-process case asserts pixels and reads these from the server's own dump.
enum class WireDeclineSite : Uint32 {
#define MGL_WIRE_DECLINE(name) name,
#include "WireDeclines.def"
#undef MGL_WIRE_DECLINE
    Count
};

inline const char* WireDeclineSiteName(WireDeclineSite site) {
    switch (site) {
#define MGL_WIRE_DECLINE(name)                                                                                         \
    case WireDeclineSite::name:                                                                                        \
        return #name;
#include "WireDeclines.def"
#undef MGL_WIRE_DECLINE
    case WireDeclineSite::Count:
        break;
    }
    return "Unnamed";
}

class WireDeclineTally {
public:
    static constexpr SizeT kSiteCount = static_cast<SizeT>(WireDeclineSite::Count);

    static void Count(WireDeclineSite site) {
        Slots()[static_cast<SizeT>(site)].fetch_add(1, std::memory_order_relaxed);
        Total().fetch_add(1, std::memory_order_relaxed);
    }

    static Uint64 Get(WireDeclineSite site) {
        return Slots()[static_cast<SizeT>(site)].load(std::memory_order_relaxed);
    }

    static Uint64 TotalDeclines() { return Total().load(std::memory_order_relaxed); }

    // The memory half (package M1's thread): a refused upload does not consume its
    // PendingUpload entry, so the entry and the level shadow behind it stay alive. These are
    // GAUGES, not counters - the last observed live depth, sampled where the refusal happens -
    // so a monotone climb across frames is the signature that a refusal is also a leak.
    static void SetPendingGauge(Uint64 entries, Uint64 bytes) {
        PendingEntries().store(entries, std::memory_order_relaxed);
        PendingBytes().store(bytes, std::memory_order_relaxed);
        Uint64 peak = PeakPendingBytes().load(std::memory_order_relaxed);
        while (bytes > peak && !PeakPendingBytes().compare_exchange_weak(peak, bytes, std::memory_order_relaxed)) {
        }
    }
    // The frame-serial floor's own counter. An "unsound serial complete" is an advance of
    // VkBufferManager's completed-frame-serial floor to a serial that ANOTHER submission still
    // in flight also carries - i.e. the renderer telling the buffer manager "frame N-1 is done"
    // while the submission holding frame N-1's buffer copies is still executing. It is what
    // lets a streamed glBufferSubData take the unordered host path and tear one draw's vertex
    // range. With a provable floor it is unreachable, so this counter is the red-once's reading:
    // non-zero on the wire arms before the fix, zero after, zero on monolith either way.
    static void CountUnsoundSerialComplete() {
        UnsoundSerial().fetch_add(1, std::memory_order_relaxed);
    }
    static Uint64 UnsoundSerialCompleteEvents() { return UnsoundSerial().load(std::memory_order_relaxed); }

    static Uint64 PendingUploadEntries() { return PendingEntries().load(std::memory_order_relaxed); }
    static Uint64 PendingUploadBytes() { return PendingBytes().load(std::memory_order_relaxed); }
    static Uint64 PeakPendingUploadBytes() { return PeakPendingBytes().load(std::memory_order_relaxed); }

    static void Reset() {
        for (SizeT i = 0; i < kSiteCount; ++i) Slots()[i].store(0, std::memory_order_relaxed);
        Total().store(0, std::memory_order_relaxed);
        UnsoundSerial().store(0, std::memory_order_relaxed);
        PendingEntries().store(0, std::memory_order_relaxed);
        PendingBytes().store(0, std::memory_order_relaxed);
        PeakPendingBytes().store(0, std::memory_order_relaxed);
    }

    // One line per site that moved, plus the pending gauges. Called at session teardown and
    // from the forced-condition harness; silent when nothing declined, so a green lane stays
    // quiet and a lane that dropped a draw cannot.
    static void Dump(const char* tag) {
        // The floor counter prints unconditionally: "zero unsound advances" is the reading the
        // red-once needs, and an absent line cannot be told from a lane that never ran.
        MGLOG_I("MGWIRE-FLOOR[%s] unsoundSerialComplete=%llu", tag,
                static_cast<unsigned long long>(UnsoundSerialCompleteEvents()));
        if (TotalDeclines() == 0 && PeakPendingUploadBytes() == 0) return;
        MGLOG_I("MGWIRE-DECLINES[%s] total=%llu pendingEntries=%llu pendingBytes=%llu peakPendingBytes=%llu", tag,
                static_cast<unsigned long long>(TotalDeclines()),
                static_cast<unsigned long long>(PendingUploadEntries()),
                static_cast<unsigned long long>(PendingUploadBytes()),
                static_cast<unsigned long long>(PeakPendingUploadBytes()));
        for (SizeT i = 0; i < kSiteCount; ++i) {
            const Uint64 value = Slots()[i].load(std::memory_order_relaxed);
            if (!value) continue;
            MGLOG_I("MGWIRE-DECLINES[%s]   %s=%llu", tag, WireDeclineSiteName(static_cast<WireDeclineSite>(i)),
                    static_cast<unsigned long long>(value));
        }
    }

private:
    static std::atomic<Uint64>* Slots() {
        static std::atomic<Uint64> slots[kSiteCount]{};
        return slots;
    }
    static std::atomic<Uint64>& Total() {
        static std::atomic<Uint64> total{0};
        return total;
    }
    static std::atomic<Uint64>& PendingEntries() {
        static std::atomic<Uint64> value{0};
        return value;
    }
    static std::atomic<Uint64>& PendingBytes() {
        static std::atomic<Uint64> value{0};
        return value;
    }
    static std::atomic<Uint64>& PeakPendingBytes() {
        static std::atomic<Uint64> value{0};
        return value;
    }
    static std::atomic<Uint64>& UnsoundSerial() {
        static std::atomic<Uint64> value{0};
        return value;
    }
};

// Every decline goes through this: the tally moves ALWAYS, the log line is once per site.
// `MGL_WIRE_DECLINE_AT(Site, "why", ...); return false;` is the statement pair that replaces
// a bare `return false;` (the few sites that call WireDeclineTally::Count directly stand
// under an MGLOG_W/E of their own; scripts/ci/wire_declines_audit.py holds them to that).
//
// A DECLINE IS NOT ALWAYS A SKIPPED DRAW, so the line does not say "draw". The Tex*, Shape* and
// Upload* rows are counted per texture SYNC: SyncTextureResourceByHandle is reached from the
// draw/dispatch texture preparation, but also from the descriptor resolver, framebuffer
// attachments, CopyTexSubImage, mipmap generation and CopyImageSubData - each of which Fatals
// right after a failed sync - and from texture readback, which falls back to the unbacked
// shadow. A draw that samples three unsyncable textures counts three; a failed attachment
// sync counts once and is then a Fatal, not a skipped draw.
#define MGL_WIRE_DECLINE_AT(site, fmt, ...)                                                                            \
    do {                                                                                                               \
        ::MobileGL::MG_Backend::DirectVulkan::WireDeclineTally::Count(                                                  \
            ::MobileGL::MG_Backend::DirectVulkan::WireDeclineSite::site);                                              \
        MGLOG_E_ONCE("Magma wire decline [" #site "]: " fmt, ##__VA_ARGS__);                                           \
    } while (0)

} // namespace MobileGL::MG_Backend::DirectVulkan

#endif // MOBILEGL_BUILD_DISAGGREGATED
