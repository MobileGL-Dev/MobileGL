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

        // One pass over /proc/self/status for a "VmHWM:" / "VmRSS:" line. The
        // values are in kB and the unit suffix is part of the line, so it is
        // parsed rather than assumed.
        std::uint64_t ProcStatusBytes(const char* key) {
#if defined(__linux__) || defined(__ANDROID__)
            std::FILE* file = std::fopen("/proc/self/status", "re");
            if (file == nullptr) {
                return 0;
            }
            const std::size_t keyLength = std::strlen(key);
            char line[256];
            std::uint64_t bytes = 0;
            while (std::fgets(line, sizeof(line), file) != nullptr) {
                if (std::strncmp(line, key, keyLength) != 0) {
                    continue;
                }
                unsigned long long kilobytes = 0;
                // The format is "<key>:\t   <number> kB". Anything else is a
                // kernel this code has not seen, and 0 ("not measured") is the
                // honest answer for it.
                if (std::sscanf(line + keyLength, ": %llu kB", &kilobytes) == 1) {
                    bytes = static_cast<std::uint64_t>(kilobytes) * 1024ull;
                }
                break;
            }
            std::fclose(file);
            return bytes;
#else
            (void)key;
            return 0;
#endif
        }
    } // namespace

    std::uint64_t ProcessPeakRssBytes() { return ProcStatusBytes("VmHWM"); }

    std::uint64_t ProcessCurrentRssBytes() { return ProcStatusBytes("VmRSS"); }

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

    RoleMemorySample SampleRoleMemory(MemoryRole role) {
        RoleMemorySample sample;
        sample.Role = role;
        sample.PeakRssBytes = ProcessPeakRssBytes();
        sample.CurrentRssBytes = ProcessCurrentRssBytes();
        sample.MappedSegmentBytes = LedgerMappedBytes(role);
        return sample;
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

        struct Spec {
            const char* name;
            std::uint64_t bytes;
        };
        const Spec specs[kSlotCount] = {
            {"mgl-cmd", sizes.CmdBytes},
            {"mgl-stage", sizes.StageBytes},
            {"mgl-reply", sizes.ReplyBytes},
            {"mgl-event", sizes.EventBytes},
        };

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

        m_owns = true;
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
        for (std::size_t index = 0; index < kSlotCount; ++index) {
            m_segments[index] = owner.m_segments[index];
        }
        m_owns = false;
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

        // SEG_STAGE carries no control page of its own: RingControl holds TWO
        // cursor triples and the stage triple is the second (Ring.h:106-109).
        m_stageBase = m_segments[1]->Data();
        m_stageCapacity = LargestPowerOfTwoAtMost(m_segments[1]->Size());

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

        if (m_cmdRingCapacity == 0 || m_stageCapacity == 0 || m_eventRingCapacity == 0 ||
            m_replyBytes == 0) {
            MGLOG_E("MG_Remote session: segment sizes leave no usable ring (cmd cap=%llu stage "
                    "cap=%llu event cap=%llu reply=%llu). A ring is the largest POWER OF TWO that "
                    "fits after the 4096 byte control page, so a segment must be strictly larger "
                    "than one page plus the smallest ring",
                    static_cast<unsigned long long>(m_cmdRingCapacity),
                    static_cast<unsigned long long>(m_stageCapacity),
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
        m_stageCapacity = 0;
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
