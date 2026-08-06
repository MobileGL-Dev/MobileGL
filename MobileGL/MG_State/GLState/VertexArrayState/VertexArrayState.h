// MobileGL - MobileGL/MG_State/GLState/VertexArrayState/VertexArrayState.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include "VertexArrayObject.h"
#include <MG_Util/Miscellany/IndexGenerator.h>

namespace MobileGL {
    namespace MG_State {
        namespace GLState {
            class VertexArrayState {
            public:
                VertexArrayState();

                const SharedPtr<VertexArrayObject>& GetVertexArrayObject(Uint index);
                void GenerateNames(Uint number, Vector<Uint>& arrays);
                void Bind(Uint index);
                const SharedPtr<VertexArrayObject>& CreateVertexArrayObject(Uint index);
                void MarkVertexArrayForDeletion(Uint index);
                Bool ValidateName(Uint index) const;
                Bool ValidateVertexArrayObject(Uint index) const;
                const SharedPtr<VertexArrayObject>& GetBoundVertexArray();
                Vector<SharedPtr<VertexArrayObject>>& GetAllVertexArrays();

            private:
                // "Nothing bound" (an out-of-range or never-created name was bound). Distinct from
                // being bound to a live slot so that a slot filled AFTER such a bind does not
                // retroactively become the bound VAO.
                static constexpr Uint kUnboundIndex = ~static_cast<Uint>(0);

                Vector<SharedPtr<VertexArrayObject>> m_vertexArrays;
                IndexGenerator<Uint> m_indexGenerator;

                // The bound VAO is represented as an INDEX into m_vertexArrays, not as an owning
                // SharedPtr copy. Chunk-style renderers rebind a different VAO before every draw,
                // and the SharedPtr store this replaces cost two atomic refcount ops per bind -
                // the single largest line of a vanilla-Minecraft draw profile (the lock-prefixed
                // refcount RMWs serialize the store buffer in the middle of command recording).
                //
                // LIFETIME INVARIANT this relies on (and which the cold paths below enforce
                // rather than assume): the object GetBoundVertexArray() refers to is kept alive
                // by its own slot in m_vertexArrays. Every path that clears or replaces a slot
                // either (a) rebinds index 0 first when it targets the bound slot
                // (MarkVertexArrayForDeletion), or (b) detaches the displaced object into
                // m_boundDetached (CreateVertexArrayObject), which then owns it and keeps
                // GetBoundVertexArray() answering with the OLD object - exactly what the previous
                // SharedPtr member did - until the next Bind drops it.
                Uint m_boundIndex = 0;
                // Cold-path ownership backstop, see above. Null in the steady state; Bind clears
                // it (one predictable branch on the hot path).
                SharedPtr<VertexArrayObject> m_boundDetached;
            };
        } // namespace GLState
    } // namespace MG_State
} // namespace MobileGL
