// MobileGL - MobileGL/MG_Backend/DirectGLES/DrawTextureReads.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include <cstdint>
#include <vector>

namespace MobileGL::MG_Backend::DirectGLES::TextureImpl {
    // One instance per native context. Slot generations distinguish reused texture names,
    // and batch stamps let us discard a batch without walking its texture slots.
    // This upload heuristic follows sampler reads from offscreen draws and the framebuffer
    // changes observed by BindCurrentFBO. Compute dispatches do not record reads here.
    class DrawTextureReads {
    public:
        void BindFramebuffer(std::uint32_t framebuffer) {
            if (m_drawFramebuffer == framebuffer) return;
            m_drawFramebuffer = framebuffer;
            EndBatch();
        }

        void DeleteFramebuffer(std::uint32_t framebuffer) {
            if (framebuffer == m_drawFramebuffer) BindFramebuffer(0);
        }

        void EndBatch() {
            m_recordedSamplers = false;
            if (++m_batch == 0) {
                m_reads.clear();
                m_batch = 1;
            }
        }

        bool NeedsSamplerWalk(std::uint64_t context, std::uint64_t views) const {
            return m_drawFramebuffer != 0 && (!m_recordedSamplers || context != m_context || views != m_views);
        }

        void RecordSamplerWalk(std::uint64_t context, std::uint64_t views) {
            m_recordedSamplers = true;
            m_context = context;
            m_views = views;
        }

        void NoteRead(std::uint32_t slot, std::uint32_t generation) {
            if (slot >= m_reads.size()) m_reads.resize(static_cast<std::size_t>(slot) + 1);
            m_reads[slot] = Stamp(generation);
        }

        bool WasRead(std::uint32_t slot, std::uint32_t generation) const {
            return m_drawFramebuffer != 0 && slot < m_reads.size() && m_reads[slot] == Stamp(generation);
        }

    private:
        std::uint64_t Stamp(std::uint32_t generation) const { return (std::uint64_t{m_batch} << 32) | generation; }

        std::uint32_t m_drawFramebuffer = 0;
        std::uint32_t m_batch = 1;
        bool m_recordedSamplers = false;
        std::uint64_t m_context = 0;
        std::uint64_t m_views = 0;
        std::vector<std::uint64_t> m_reads;
    };
} // namespace MobileGL::MG_Backend::DirectGLES::TextureImpl
