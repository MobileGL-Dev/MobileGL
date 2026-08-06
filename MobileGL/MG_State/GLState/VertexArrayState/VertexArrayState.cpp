// MobileGL - MobileGL/MG_State/GLState/VertexArrayState/VertexArrayState.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "VertexArrayState.h"

namespace MobileGL::MG_State::GLState {
    VertexArrayState::VertexArrayState() : m_indexGenerator(1024, 1) {
        // Generate default VAO at index 0, which is not valid in core profile, but still remains for
        // compatibility reasons.
        m_indexGenerator.Insert(0);
        auto defaultVAO = MakeShared<VertexArrayObject>(0);
        m_vertexArrays.push_back(defaultVAO);
        m_boundVertexArray = defaultVAO;
    }

    const SharedPtr<VertexArrayObject>& VertexArrayState::GetVertexArrayObject(Uint index) {
        if (index >= m_vertexArrays.size()) {
            // FIXME: report a GL error here
            static SharedPtr<VertexArrayObject> nullVertexArrayObject = nullptr;
            return nullVertexArrayObject;
        }

        return m_vertexArrays[index];
    }

    void VertexArrayState::GenerateNames(Uint number, Vector<Uint>& arrays) {
        arrays.resize(number);
        m_indexGenerator.Generate(number, arrays.data());
    }

    void VertexArrayState::Bind(Uint index) {
        const auto& vertexArray = GetVertexArrayObject(index);
        // Re-binding the already-current VAO is a per-batch habit of Blaze3D-style renderers;
        // skip the shared_ptr store (two atomic refcount ops) when nothing changes.
        if (vertexArray == m_boundVertexArray) return;
        m_boundVertexArray = vertexArray;
    }

    const SharedPtr<VertexArrayObject>& VertexArrayState::CreateVertexArrayObject(Uint index) {
        if (index >= m_vertexArrays.size()) {
            // power-of-2 reallocation
            m_vertexArrays.reserve(std::bit_ceil(index + 1));
            m_vertexArrays.resize(index + 1, nullptr);
        }
        auto& vao = m_vertexArrays[index];
        vao = MakeShared<VertexArrayObject>(index);
        return vao;
    }

    void VertexArrayState::MarkVertexArrayForDeletion(Uint index) {
        if (m_indexGenerator.IsValid(index)) {
            if (m_boundVertexArray && m_boundVertexArray->GetExternalIndex() == index) {
                m_boundVertexArray = GetVertexArrayObject(0);
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
        return m_boundVertexArray;
    }

    Vector<SharedPtr<VertexArrayObject>>& VertexArrayState::GetAllVertexArrays() {
        return m_vertexArrays;
    }
} // namespace MobileGL::MG_State::GLState
