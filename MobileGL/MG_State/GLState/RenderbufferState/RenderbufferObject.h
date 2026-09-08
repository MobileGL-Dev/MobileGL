// MobileGL - MobileGL/MG_State/GLState/RenderbufferState/RenderbufferObject.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_Util/Math/VectorTypes.h>
#include <MG_State/GLState/TextureState/TextureEnum.h>

namespace MobileGL {
    enum class RenderbufferTarget {
        Renderbuffer,
        RenderbufferTargetCount,
        Unknown = -1
    };

    namespace MG_State {
        namespace GLState {
            class RenderbufferObject {
            public:
                using TargetEnum = RenderbufferTarget;

                RenderbufferObject(Uint externalIndex);
#if MOBILEGL_PIPE_PUSH
                // P2 step e2. Out of line, and declared only where there is a notice to raise:
                // in a pull build this class stays trivially destructible, which is what keeps
                // the pull build's symbol set byte-for-byte the pre-P2 one (G1).
                ~RenderbufferObject();
#endif

                Uint GetExternalIndex() const;
                void SetInternalFormat(TextureInternalFormat format);
                void AllocateStorage(IntVec2 size);
                void SetSamples(Int samples);
                Int GetWidth() const;
                Int GetHeight() const;
                TextureInternalFormat GetInternalFormat() const;
                Bool IsAllocated() const;
                const ComponentSizes& GetComponentSizes() const;
                Int GetRedSize() const;
                Int GetGreenSize() const;
                Int GetBlueSize() const;
                Int GetAlphaSize() const;
                Int GetDepthSize() const;
                Int GetStencilSize() const;
                Int GetSamples() const;
                // Globally-unique, never-reused id for THIS object's lifetime - same contract
                // and same motivation as BufferObject::GetLifetimeId(),
                // ProgramObject::GetLifetimeId() and VertexArrayObject::GetLifetimeId(). A
                // backend that folds a renderbuffer's IDENTITY into a cache key must use this,
                // never the GL name (LIFO-recycled by glGenRenderbuffers) and never the heap
                // address (recycled by the allocator): both let a deleted-and-recreated
                // renderbuffer answer to a dead one's cache entry.
                Uint64 GetLifetimeId() const { return m_lifetimeId; }

            private:
                static Uint64 AllocateLifetimeId();
#if MOBILEGL_PIPE_PUSH
                // P4a D-D2: resource_respecify, from every storage-defining setter. Non-virtual
                // and push-only, so the pull build's object layout is untouched (P4a's
                // admitted-resize set is EMPTY); defined in RenderbufferObject.cpp, which is the
                // one translation unit that includes the client emitter.
                void PipePublishDescriptor();
#endif

                Uint m_externalIndex = 0;
                const Uint64 m_lifetimeId = AllocateLifetimeId();
                TextureInternalFormat m_internalFormat = TextureInternalFormat::RGBA;
                Int m_width = 0;
                Int m_height = 0;
                Int m_samples = 0;
                Bool m_allocated = false;
                ComponentSizes m_componentSizes;
            };
        } // namespace GLState
    } // namespace MG_State
} // namespace MobileGL
