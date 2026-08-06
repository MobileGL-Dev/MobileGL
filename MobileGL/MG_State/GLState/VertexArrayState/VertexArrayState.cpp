// MobileGL - MobileGL/MG_State/GLState/VertexArrayState/VertexArrayState.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "VertexArrayState.h"

namespace MobileGL::MG_State::GLState {
    namespace {
        // Shared "nothing bound" answer for GetBoundVertexArray/GetVertexArrayObject.
        // Function-local statics carry a guard check per access; this one is
        // constant-initialized and lives on the hot per-draw path.
        const SharedPtr<VertexArrayObject> kNullVertexArrayObject = nullptr;
    } // namespace

    VertexArrayState::VertexArrayState() : m_indexGenerator(1024, 1) {
        // Generate default VAO at index 0, which is not valid in core profile, but still remains for
        // compatibility reasons.
        m_indexGenerator.Insert(0);
        auto defaultVAO = MakeShared<VertexArrayObject>(0);
        m_vertexArrays.push_back(defaultVAO);
        m_boundIndex = 0;
    }

    const SharedPtr<VertexArrayObject>& VertexArrayState::GetVertexArrayObject(Uint index) {
        if (index >= m_vertexArrays.size()) {
            // FIXME: report a GL error here
            return kNullVertexArrayObject;
        }

        return m_vertexArrays[index];
    }

    void VertexArrayState::GenerateNames(Uint number, Vector<Uint>& arrays) {
        arrays.resize(number);
        m_indexGenerator.Generate(number, arrays.data());
    }

    void VertexArrayState::Bind(Uint index) {
        // Per-draw-batch hot path (Blaze3D-style renderers rebind a different VAO before
        // every draw): store only the slot index - no SharedPtr copy, no refcount atomics.
        // The bound object's lifetime is guaranteed by its slot (see the invariant note on
        // m_boundIndex in the header).
        if (m_boundDetached) [[unlikely]] {
            // A cold path displaced the previously bound object out of its slot; this Bind
            // supersedes it, exactly like the old SharedPtr member being overwritten.
            m_boundDetached = nullptr;
        }
        // Match the previous semantics exactly: binding an out-of-range name, or a name
        // whose slot holds no object, left the old SharedPtr member null - resolve that
        // NOW, so a slot created later does not silently become bound.
        m_boundIndex =
            (index < m_vertexArrays.size() && m_vertexArrays[index] != nullptr) ? index : kUnboundIndex;
    }

    const SharedPtr<VertexArrayObject>& VertexArrayState::CreateVertexArrayObject(Uint index) {
        if (index >= m_vertexArrays.size()) {
            // power-of-2 reallocation
            m_vertexArrays.reserve(std::bit_ceil(index + 1));
            m_vertexArrays.resize(index + 1, nullptr);
        }
        auto& vao = m_vertexArrays[index];
        if (index == m_boundIndex && vao != nullptr && !m_boundDetached) {
            // Replacing the bound slot's live object: keep the OLD object alive and bound
            // (that is what the previous SharedPtr member provided) until the next Bind.
            // Unreachable through the GL entry points today - bind-time creation only fills
            // empty slots and generated names are never in use - but the invariant is
            // enforced here, not assumed.
            m_boundDetached = std::move(vao);
        }
        vao = MakeShared<VertexArrayObject>(index);
        return vao;
    }

    void VertexArrayState::MarkVertexArrayForDeletion(Uint index) {
        if (m_indexGenerator.IsValid(index)) {
            // "Deleting the bound VAO rebinds the default VAO" needs the same answer the old
            // SharedPtr compare gave: either the live bound slot is the one being deleted, or
            // the bound object is a detached one that carries this external index.
            const Bool deletingBound = m_boundDetached
                ? m_boundDetached->GetExternalIndex() == index
                : (m_boundIndex == index && m_boundIndex != kUnboundIndex);
            if (deletingBound) {
                m_boundDetached = nullptr;
                m_boundIndex = 0; // the default VAO's slot always exists
                if (index == 0) {
                    // Deleting slot 0 while it is bound (unreachable via the GL entry
                    // points, which filter name 0): the old SharedPtr member kept the
                    // object alive and bound across the slot null-out below; detach it
                    // to preserve that.
                    m_boundDetached = m_vertexArrays[0];
                }
            }

            if (ValidateVertexArrayObject(index)) {
                m_vertexArrays[index] = nullptr;
            }

            m_indexGenerator.Delete(index);
        }
        // FIXME: report GL error here?
    }

    Bool VertexArrayState::ValidateName(Uint index) const {
        return m_indexGenerator.IsValid(index);
    }

    Bool VertexArrayState::ValidateVertexArrayObject(Uint index) const {
        return index < m_vertexArrays.size() && m_vertexArrays[index] != nullptr;
    }

    const SharedPtr<VertexArrayObject>& VertexArrayState::GetBoundVertexArray() {
        // NOTE: like GetVertexArrayObject, the returned reference is a slot reference and
        // must not be held across CreateVertexArrayObject (vector growth) - existing
        // callers bind/create first and only then take the reference.
        if (m_boundDetached) [[unlikely]] {
            return m_boundDetached;
        }
        if (m_boundIndex < m_vertexArrays.size()) {
            return m_vertexArrays[m_boundIndex];
        }
        return kNullVertexArrayObject;
    }

    Vector<SharedPtr<VertexArrayObject>>& VertexArrayState::GetAllVertexArrays() {
        return m_vertexArrays;
    }
} // namespace MobileGL::MG_State::GLState
