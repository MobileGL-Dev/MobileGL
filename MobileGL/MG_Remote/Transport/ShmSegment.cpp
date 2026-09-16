// MobileGL - MobileGL/MG_Remote/Transport/ShmSegment.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// Platform-independent half of ShmSegment. The create/map/close bodies live in
// ShmSegmentPosix.cpp and ShmSegmentWin32.cpp.
//
// P5 adds two things that are about a SET of segments rather than about one:
// SessionSegments (the four a session owns, and the ring geometry derived from
// them) and the role memory ledger that t1 reports against.

#include "ShmSegment.h"

#include "RoleMemory.h"
#include "SessionRings.h"

#include <MG_Util/Debug/Log.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <utility>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace MobileGL::MG_Remote::Transport {

    ShmSegment::~ShmSegment() { Close(); }

    ShmSegment::ShmSegment(ShmSegment&& other) noexcept { Steal(std::move(other)); }

    ShmSegment& ShmSegment::operator=(ShmSegment&& other) noexcept {
        if (this != &other) {
            Close();
            Steal(std::move(other));
        }
        return *this;
    }

    void ShmSegment::Steal(ShmSegment&& other) noexcept {
        std::memcpy(m_name, other.m_name, sizeof(m_name));
        m_mapping = other.m_mapping;
        m_nativeHandle = other.m_nativeHandle;
        m_size = other.m_size;
        m_fd = other.m_fd;
        m_readOnly = other.m_readOnly;

        std::memset(other.m_name, 0, sizeof(other.m_name));
        other.m_mapping = nullptr;
        other.m_nativeHandle = nullptr;
        other.m_size = 0;
        other.m_fd = -1;
        other.m_readOnly = false;
    }

    bool ShmSegment::Valid() const { return m_size != 0 && (m_fd >= 0 || m_nativeHandle != nullptr); }

    // =======================================================================
    // P5: the role memory ledger and the VmHWM sample
    // =======================================================================

    namespace {
        constexpr std::size_t kMemoryRoleCount = static_cast<std::size_t>(MemoryRole::kMemoryRoleCount);

        std::atomic<std::uint64_t>& LedgerSlot(MemoryRole role) {
            static std::atomic<std::uint64_t> ledger[kMemoryRoleCount];
            const std::size_t index = static_cast<std::size_t>(role);
            return ledger[index < kMemoryRoleCount ? index : 0];
        }

        const char* RoleName(MemoryRole role) {
            return role == MemoryRole::Server ? "server" : "client";
        }

        // "<key>:\t   <number> kB" -> bytes. The unit suffix is part of the
        // line, so it is parsed rather than assumed; anything else is a kernel
        // this code has not seen, and 0 ("not measured") is the honest answer.
        bool ParseKilobyteLine(const char* line, const char* key, std::uint64_t* outBytes) {
            const std::size_t keyLength = std::strlen(key);
            if (std::strncmp(line, key, keyLength) != 0) {
                return false;
            }
            unsigned long long kilobytes = 0;
            if (std::sscanf(line + keyLength, ": %llu kB", &kilobytes) == 1) {
                *outBytes = static_cast<std::uint64_t>(kilobytes) * 1024ull;
            }
            return true;
        }

        // The process-wide running peak SampleRoleMemory folds into. One per
        // process, like VmHWM itself; under inproc both roles share it and the
        // log line says so.
        std::atomic<std::uint64_t>& ProcessRunningPeak() {
            static std::atomic<std::uint64_t> peak{0};
            return peak;
        }
    } // namespace

    // ONE PASS FOR BOTH KEYS - see RoleMemory.h. A pass per key was the wave-1
    // shape and it is what GitHub run 35079459114 caught: the second fopen grew
    // RSS past the VmHWM the first pass had reported, because the kernel only
    // stores hiwater_rss when RSS is about to drop and reports max(stored, now)
    // otherwise, so the two reads were snapshots of different "now"s.
    void ProcessRssBytes(std::uint64_t* outPeakBytes, std::uint64_t* outCurrentBytes) {
        std::uint64_t peak = 0;
        std::uint64_t current = 0;
#if defined(__linux__) || defined(__ANDROID__)
        std::FILE* file = std::fopen("/proc/self/status", "re");
        if (file != nullptr) {
            char line[256];
            bool sawPeak = false;
            bool sawCurrent = false;
            while ((!sawPeak || !sawCurrent) && std::fgets(line, sizeof(line), file) != nullptr) {
                if (!sawPeak && ParseKilobyteLine(line, "VmHWM", &peak)) {
                    sawPeak = true;
                } else if (!sawCurrent && ParseKilobyteLine(line, "VmRSS", &current)) {
                    sawCurrent = true;
                }
            }
            std::fclose(file);
        }
#endif
        if (outPeakBytes != nullptr) {
            *outPeakBytes = peak;
        }
        if (outCurrentBytes != nullptr) {
            *outCurrentBytes = current;
        }
    }

    std::uint64_t ProcessPeakRssBytes() {
        std::uint64_t peak = 0;
        ProcessRssBytes(&peak, nullptr);
        return peak;
    }

    std::uint64_t ProcessCurrentRssBytes() {
        std::uint64_t current = 0;
        ProcessRssBytes(nullptr, &current);
        return current;
    }

    void LedgerAddSegment(MemoryRole role, std::uint64_t bytes) {
        LedgerSlot(role).fetch_add(bytes, std::memory_order_relaxed);
    }

    void LedgerRemoveSegment(MemoryRole role, std::uint64_t bytes) {
        std::atomic<std::uint64_t>& slot = LedgerSlot(role);
        const std::uint64_t current = slot.load(std::memory_order_relaxed);
        // Clamped rather than wrapped: a double-unbook would otherwise report a
        // role holding sixteen exabytes, which is a number nobody reads as a bug.
        slot.store(bytes > current ? 0 : current - bytes, std::memory_order_relaxed);
    }

    std::uint64_t LedgerMappedBytes(MemoryRole role) {
        return LedgerSlot(role).load(std::memory_order_relaxed);
    }

    std::uint64_t LedgerMappedBytesAllRoles() {
        std::uint64_t total = 0;
        for (std::size_t index = 0; index < kMemoryRoleCount; ++index) {
            total += LedgerSlot(static_cast<MemoryRole>(index)).load(std::memory_order_relaxed);
        }
        return total;
    }

    RoleMemorySample SampleRoleMemoryInto(std::atomic<std::uint64_t>& runningPeak, MemoryRole role,
                                          std::uint64_t kernelPeakRssBytes,
                                          std::uint64_t kernelCurrentRssBytes) {
        // max(running, kernel peak, kernel current), as a CAS loop: two threads
        // sampling at once (the GL thread and the apply thread both log memory
        // at phase boundaries) must not let a lower value overwrite a higher.
        const std::uint64_t observed =
            kernelPeakRssBytes > kernelCurrentRssBytes ? kernelPeakRssBytes : kernelCurrentRssBytes;
        std::uint64_t peak = runningPeak.load(std::memory_order_relaxed);
        while (observed > peak &&
               !runningPeak.compare_exchange_weak(peak, observed, std::memory_order_relaxed)) {
        }
        RoleMemorySample sample;
        sample.Role = role;
        // THE RUNNING MAXIMUM, never the kernel's peak verbatim - RoleMemory.h
        // says why, and SessionTest's stubbed-reader control is the gate on it.
        sample.PeakRssBytes = observed > peak ? observed : peak;
        sample.CurrentRssBytes = kernelCurrentRssBytes;
        sample.MappedSegmentBytes = LedgerMappedBytes(role);
        return sample;
    }

    RoleMemorySample SampleRoleMemory(MemoryRole role) {
        std::uint64_t kernelPeak = 0;
        std::uint64_t kernelCurrent = 0;
        ProcessRssBytes(&kernelPeak, &kernelCurrent);
        return SampleRoleMemoryInto(ProcessRunningPeak(), role, kernelPeak, kernelCurrent);
    }

    void LogRoleMemory(const char* phase, const RoleMemorySample& sample) {
        // INFO, not DEBUG: t1 greps this out of a lane log and MGLOG_D is compiled out at the
        // INFO level every P5 lane builds at. It is a handful of lines per session - the
        // handshake, the first frame and teardown - so it is not per-frame noise either.
        //
        // VmHWM is the PROCESS's, so under inproc both roles report the same
        // number and only the ledger differs. The line says so rather than
        // leaving a reader to work out why two roles have one peak.
        MGLOG_I("MG_Remote memory[%s/%s]: peakRss=%llu currentRss=%llu roleMapped=%llu "
                "allRolesMapped=%llu (peakRss is the PROCESS's; under inproc both roles share it)",
                phase == nullptr ? "?" : phase, RoleName(sample.Role),
                static_cast<unsigned long long>(sample.PeakRssBytes),
                static_cast<unsigned long long>(sample.CurrentRssBytes),
                static_cast<unsigned long long>(sample.MappedSegmentBytes),
                static_cast<unsigned long long>(LedgerMappedBytesAllRoles()));
    }

    // =======================================================================
    // P5: SessionSegments
    // =======================================================================

    namespace {
        constexpr std::size_t kSlotCount =
            static_cast<std::size_t>(SessionSegmentSlot::kSessionSegmentCount);

        std::size_t SlotIndex(SessionSegmentSlot slot) {
            const std::size_t index = static_cast<std::size_t>(slot);
            return index < kSlotCount ? index : 0;
        }
    } // namespace

    SessionSegments::~SessionSegments() { Close(); }

    MobileGLResult SessionSegments::Create(const SessionSegmentSizes& sizes, MemoryRole role) {
        Close();
        // Set BEFORE the loop, so that Close() on a failure INSIDE it really
        // closes what has already been created: Close only walks m_owned when
        // m_owns is true, and setting it afterwards left a failure at segment 3
        // holding segments 0-2's descriptors and mappings open with Valid()
        // false, against ShmSegment.h:66's "unmaps and releases the descriptor".
        m_owns = true;

        struct Spec {
            const char* name;
            std::uint64_t bytes;
        };
        // The sizes are RING sizes; a segment that carries a control page at its
        // head is that much bigger. SEG_STAGE drives the SECOND cursor triple of
        // SEG_CMD's page and SEG_REPLY is not a ring at all, so neither of those
        // two grows.
        const Spec specs[kSlotCount] = {
            {"mgl-cmd", SegmentBytesForRing(sizes.CmdRingBytes)},
            {"mgl-stage", sizes.StageBytes},
            {"mgl-reply", sizes.ReplyBytes},
            {"mgl-event", SegmentBytesForRing(sizes.EventRingBytes)},
        };
        for (std::size_t index = 0; index < kSlotCount; ++index) {
            if (specs[index].bytes == 0) {
                MGLOG_E("MG_Remote session: segment %s was asked for a ring size that cannot be "
                        "made into one (a ring is a power of two between %llu and %llu bytes)",
                        specs[index].name, static_cast<unsigned long long>(kMinRingCapacity),
                        static_cast<unsigned long long>(kMaxRingCapacity));
                Close();
                return MOBILEGL_ERR_INVALID_ARGUMENT;
            }
        }

        for (std::size_t index = 0; index < kSlotCount; ++index) {
            const MobileGLResult created =
                ShmSegment::Create(specs[index].name, specs[index].bytes, m_owned[index]);
            if (created != MOBILEGL_OK) {
                MGLOG_E("MG_Remote session: could not create segment %s of %llu bytes (rc=%d)",
                        specs[index].name, static_cast<unsigned long long>(specs[index].bytes),
                        static_cast<int>(created));
                Close();
                return created;
            }
            // Read/write on both roles under inproc: they are the same mapping.
            // P6's read-only peer view is a property of the ADOPT path, not of
            // this one, and pretending otherwise here would give the inproc lane
            // a protection the spawn lane does not reproduce.
            const MobileGLResult mapped = m_owned[index].Map(false);
            if (mapped != MOBILEGL_OK) {
                MGLOG_E("MG_Remote session: could not map segment %s (rc=%d)", specs[index].name,
                        static_cast<int>(mapped));
                Close();
                return mapped;
            }
        }

        m_replySlotCount = sizes.ReplySlotCount;
        DeriveViews();
        if (!m_valid) {
            Close();
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }

        // Both control pages, zeroed with their generations at 1. The owner does
        // this exactly once; the inproc peer must NOT, or it would zero the
        // cursors out from under whoever is already using them.
        InitRingControl(*m_cmdControl);
        InitRingControl(*m_eventControl);

        m_role = role;
        LedgerAddSegment(role, m_mappedBytes);
        m_booked = true;
        return MOBILEGL_OK;
    }

    MobileGLResult SessionSegments::AttachInProcess(SessionSegments& owner, MemoryRole role) {
        Close();
        if (!owner.Valid()) {
            return MOBILEGL_ERR_NOT_INITIALIZED;
        }
#if !defined(_WIN32)
        // A REAL SECOND MAPPING, not an alias. dup + Adopt + Map is byte for byte
        // the call sequence P6's SCM_RIGHTS client runs, so mapping, the fstat
        // size check inside Adopt (ShmSegmentPosix.cpp:119-141), alignment and the
        // peer's own lifetime are all exercised now rather than on the day the
        // second process appears. Aliasing the owner's ShmSegment objects would
        // leave the attach half untested for exactly the reason this file refuses
        // to allocate the rings with new[].
        m_owns = true;
        for (std::size_t index = 0; index < kSlotCount; ++index) {
            const ShmSegment* theirs = owner.m_segments[index];
            const int duplicate = theirs == nullptr ? -1 : ::dup(theirs->Fd());
            if (duplicate < 0) {
                MGLOG_E("MG_Remote session: could not dup the owner's descriptor for segment %zu",
                        index);
                Close();
                return MOBILEGL_ERR_INVALID_ARGUMENT;
            }
            // Adopt takes ownership of `duplicate` on success only.
            const MobileGLResult adopted =
                ShmSegment::Adopt(duplicate, theirs->Size(), m_owned[index]);
            if (adopted != MOBILEGL_OK) {
                ::close(duplicate);
                Close();
                return adopted;
            }
            // Read/write: under inproc the client writes SEG_CMD and SEG_STAGE and
            // reads SEG_REPLY and SEG_EVENT, and one ShmSegment maps the whole
            // thing one way. The per-segment read-only peer view is P6's, where
            // the roles are separable.
            const MobileGLResult mapped = m_owned[index].Map(false);
            if (mapped != MOBILEGL_OK) {
                Close();
                return mapped;
            }
        }
#else
        // Windows has no Adopt (ShmSegment::Adopt is POSIX-only; a Windows peer
        // resolves the section by the name carried in SegmentRef). Alias, and say
        // so: this arm does not exercise the attach path P6 replaces.
        for (std::size_t index = 0; index < kSlotCount; ++index) {
            m_segments[index] = owner.m_segments[index];
        }
        m_owns = false;
#endif
        m_replySlotCount = owner.m_replySlotCount;
        DeriveViews();
        if (!m_valid) {
            Close();
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        m_role = role;
        // Booked under this role as well, and that double-counting is the point:
        // the pages are shared under inproc and are NOT shared under spawn, so
        // the per-role numbers are what t1 subtracts with.
        LedgerAddSegment(role, m_mappedBytes);
        m_booked = true;
        return MOBILEGL_OK;
    }

    void SessionSegments::DeriveViews() {
        m_valid = false;
        if (m_owns) {
            for (std::size_t index = 0; index < kSlotCount; ++index) {
                m_segments[index] = &m_owned[index];
            }
        }
        for (std::size_t index = 0; index < kSlotCount; ++index) {
            if (m_segments[index] == nullptr || m_segments[index]->Data() == nullptr) {
                return;
            }
        }

        auto* cmdBase = static_cast<std::uint8_t*>(m_segments[0]->Data());
        m_cmdControl = reinterpret_cast<RingControl*>(cmdBase);
        m_cmdRingBase = cmdBase + sizeof(RingControl);
        m_cmdRingCapacity = RingCapacityForSegment(m_segments[0]->Size());

        // SEG_STAGE IS NOT A RING: no control page, no cursor triple, no power-of-
        // two rounding. Package w1's encoder owns it as a linear allocator that
        // reclaims on retiredSeq, so the whole mapping is usable bytes.
        m_stageBase = m_segments[1]->Data();
        m_stageBytes = m_segments[1]->Size();

        m_replyBase = m_segments[2]->Data();
        m_replyBytes = m_segments[2]->Size();

        auto* eventBase = static_cast<std::uint8_t*>(m_segments[3]->Data());
        m_eventSegmentBase = eventBase;
        m_eventControl = reinterpret_cast<RingControl*>(eventBase);
        m_eventRingBase = eventBase + sizeof(RingControl);
        m_eventRingCapacity = RingCapacityForSegment(m_segments[3]->Size());

        m_mappedBytes = 0;
        for (std::size_t index = 0; index < kSlotCount; ++index) {
            m_mappedBytes += m_segments[index]->Size();
        }

        if (m_cmdRingCapacity == 0 || m_stageBytes == 0 || m_eventRingCapacity == 0 ||
            m_replyBytes == 0) {
            MGLOG_E("MG_Remote session: segment sizes leave no usable ring (cmd cap=%llu stage "
                    "cap=%llu event cap=%llu reply=%llu). A ring is the largest POWER OF TWO that "
                    "fits after the 4096 byte control page, so a segment must be strictly larger "
                    "than one page plus the smallest ring",
                    static_cast<unsigned long long>(m_cmdRingCapacity),
                    static_cast<unsigned long long>(m_stageBytes),
                    static_cast<unsigned long long>(m_eventRingCapacity),
                    static_cast<unsigned long long>(m_replyBytes));
            return;
        }
        m_valid = true;
    }

    void SessionSegments::Close() {
        if (m_booked) {
            LedgerRemoveSegment(m_role, m_mappedBytes);
            m_booked = false;
        }
        if (m_owns) {
            for (ShmSegment& segment : m_owned) {
                segment.Close();
            }
        }
        for (std::size_t index = 0; index < kSlotCount; ++index) {
            m_segments[index] = nullptr;
        }
        m_cmdControl = nullptr;
        m_cmdRingBase = nullptr;
        m_cmdRingCapacity = 0;
        m_stageBase = nullptr;
        m_stageBytes = 0;
        m_replyBase = nullptr;
        m_replyBytes = 0;
        m_eventControl = nullptr;
        m_eventSegmentBase = nullptr;
        m_eventRingBase = nullptr;
        m_eventRingCapacity = 0;
        m_mappedBytes = 0;
        m_owns = false;
        m_valid = false;
    }

    std::uint64_t SessionSegments::AnnouncedSize(SessionSegmentSlot slot) const {
        const ShmSegment* segment = m_segments[SlotIndex(slot)];
        return segment == nullptr ? 0 : segment->Size();
    }

    const char* SessionSegments::AnnouncedName(SessionSegmentSlot slot) const {
        const ShmSegment* segment = m_segments[SlotIndex(slot)];
        return segment == nullptr ? "" : segment->Name();
    }

    int SessionSegments::DescriptorFor(SessionSegmentSlot slot) const {
        const ShmSegment* segment = m_segments[SlotIndex(slot)];
        return segment == nullptr ? -1 : segment->Fd();
    }

} // namespace MobileGL::MG_Remote::Transport
