// MobileGL - MobileGL/MG_State/GLState/BufferState/BufferState.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "BufferState.h"

namespace MobileGL::MG_State::GLState {
    BufferState::BufferState() : m_indexGenerator(1024, 1) {
        for (SizeT i = 0; i < m_bindingSlots.size(); ++i) {
            m_bindingSlots[i] = BindingSlot<BufferObject>(GlobalBufferTargets[i]);
        }
    }

    const SharedPtr<BufferObject>& BufferState::GetBufferObject(Uint index) {
        auto it = m_bufferObjects.find(index);
        if (it != m_bufferObjects.end()) {
            return it->second;
        }
        static SharedPtr<BufferObject> nullBufferObject = nullptr;
        return nullBufferObject;
    }

    void BufferState::GenerateNames(Uint number, Vector<Uint>& buffers) {
        buffers.resize(number);
        m_indexGenerator.Generate(number, buffers.data());
    }

    const SharedPtr<BufferObject>& BufferState::CreateBufferObject(Uint index) {
        auto& bufferObj = m_bufferObjects[index];
        if (!bufferObj) {
            bufferObj = MakeShared<BufferObject>(index);
        }
        return bufferObj;
    }

    BindingSlot<BufferObject>& BufferState::GetBindingSlot(BufferTarget target) {
        for (auto& bindingSlot : m_bindingSlots) {
            if (bindingSlot.GetTarget() == target) {
                return bindingSlot;
            }
        }
        MOBILEGL_ASSERT(false, "Invalid BufferTarget enum value: %d", static_cast<int>(target));
        return m_bindingSlots[0];
    }

    void BufferState::MarkBufferObjectForDeletion(Uint index) {
        if (m_indexGenerator.IsValid(index)) {
            auto it = m_bufferObjects.find(index);
            if (it != m_bufferObjects.end()) {
                for (auto& bindingSlot : m_bindingSlots) {
                    if (bindingSlot.GetBoundObject() == it->second) {
                        bindingSlot.Bind(nullptr);
                    }
                }
                for (auto& bindingPointArray : m_bufferBindPointTargets) {
                    for (auto& bindingPoint : bindingPointArray) {
                        if (bindingPoint.GetBoundObject() == it->second) {
                            bindingPoint.Bind(nullptr);
                            bindingPoint.ClearRange();
                        }
                    }
                }
                // Key-based erase skips FastSTL's successor-iterator scan, which is
                // pure overhead here and dominates delete-heavy frames.
                m_bufferObjects.erase(index);
            }
            m_indexGenerator.Delete(index);
        }
    }

    Bool BufferState::ValidateName(Uint index) const {
        return m_indexGenerator.IsValid(index);
    }

    Bool BufferState::ValidateBufferObject(Uint index) const {
        return m_bufferObjects.find(index) != m_bufferObjects.end();
    }

    BindingSlotRange1D<BufferObject>& BufferState::GetBindingPoint(BufferTarget target, Uint index) {
        for (SizeT i = 0; i < BufferBindPointTargets.size(); ++i) {
            if (BufferBindPointTargets[i] == target) {
                return m_bufferBindPointTargets[i][index];
            }
        }
        MOBILEGL_ASSERT(false, "Invalid BufferTarget enum value for binding point: %d", static_cast<int>(target));
        return m_bufferBindPointTargets[0][index];
    }
} // namespace MobileGL::MG_State::GLState
