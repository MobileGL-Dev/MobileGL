// MobileGL - MobileGL/MG_Remote/Transport/ShmSegment.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// Platform-independent half of ShmSegment. The create/map/close bodies live in
// ShmSegmentPosix.cpp and ShmSegmentWin32.cpp.

#include "ShmSegment.h"

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

} // namespace MobileGL::MG_Remote::Transport
