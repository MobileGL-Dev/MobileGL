// MobileGL - MobileGL/MG_State/GLState/RenderbufferState/RenderbufferObject.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "RenderbufferObject.h"
#include <MG_Util/Metrics/TextureMetrics.h>

#include <atomic>

namespace MobileGL {
    namespace MG_State {
        namespace GLState {
            namespace {
                // Starts at 1 so a zero-initialized cache slot can never carry a live
                // renderbuffer's id.
                std::atomic<Uint64> g_nextRenderbufferLifetimeId{1};
            }

            Uint64 RenderbufferObject::AllocateLifetimeId() {
                return g_nextRenderbufferLifetimeId.fetch_add(1, std::memory_order_relaxed);
            }

            RenderbufferObject::RenderbufferObject(Uint externalIndex) : m_externalIndex(externalIndex) {}

            Uint RenderbufferObject::GetExternalIndex() const {
                return m_externalIndex;
            }

            Int RenderbufferObject::GetWidth() const {
                return m_width;
            }

            Int RenderbufferObject::GetHeight() const {
                return m_height;
            }

            TextureInternalFormat RenderbufferObject::GetInternalFormat() const {
                return m_internalFormat;
            }

            Bool RenderbufferObject::IsAllocated() const {
                return m_allocated;
            }

            Int RenderbufferObject::GetRedSize() const {
                return m_componentSizes.Red;
            }

            Int RenderbufferObject::GetGreenSize() const {
                return m_componentSizes.Green;
            }

            Int RenderbufferObject::GetBlueSize() const {
                return m_componentSizes.Blue;
            }

            Int RenderbufferObject::GetAlphaSize() const {
                return m_componentSizes.Alpha;
            }

            Int RenderbufferObject::GetDepthSize() const {
                return m_componentSizes.Depth;
            }

            Int RenderbufferObject::GetStencilSize() const {
                return m_componentSizes.Stencil;
            }

            Int RenderbufferObject::GetSamples() const {
                return m_samples;
            }

            const ComponentSizes& RenderbufferObject::GetComponentSizes() const {
                return m_componentSizes;
            }

            void RenderbufferObject::SetInternalFormat(TextureInternalFormat format) {
                m_internalFormat = format;
                m_componentSizes = MG_Util::GetComponentSizesForInternalFormat(format);
            }

            void RenderbufferObject::AllocateStorage(IntVec2 size) {
                m_width = size.x();
                m_height = size.y();
                m_allocated = true;
            }

            void RenderbufferObject::SetSamples(Int samples) {
                m_samples = samples;
            }
        } // namespace GLState
    } // namespace MG_State
} // namespace MobileGL
