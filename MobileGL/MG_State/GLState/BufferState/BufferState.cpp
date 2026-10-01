// MobileGL - MobileGL/MG_State/GLState/BufferState/BufferState.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "BufferState.h"

namespace MobileGL::MG_State::GLState {
    BufferState::BufferState() {
        for (SizeT i = 0; i < m_bindingSlots.size(); ++i) {
            m_bindingSlots[i] = BindingSlot<BufferObject>(GlobalBufferTargets[i]);
        }
        for (SizeT i = 0; i < m_touchedBindPointCount.size(); ++i) {
            m_touchedBindPointCount[i] = 0;
        }
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

    void BufferState::UnbindBufferObject(const SharedPtr<BufferObject>& bufferObject) {
        if (!bufferObject) return;
        for (auto& bindingSlot : m_bindingSlots) {
            if (bindingSlot.GetBoundObject() == bufferObject) {
                bindingSlot.Bind(nullptr);
            }
        }
        for (auto& bindingPointArray : m_bufferBindPointTargets) {
            for (auto& bindingPoint : bindingPointArray) {
                if (bindingPoint.GetBoundObject() == bufferObject) {
                    bindingPoint.Bind(nullptr);
                    bindingPoint.ClearRange();
#if MOBILEGL_PIPE_PUSH
                    // An UNBIND, and the one writer of an indexed point that is not an
                    // entry point (GL_Buffer.cpp's BindBuffer{Base,Range}_State and
                    // Core.cpp's transform-feedback writers bump their own): without the
                    // bump the target's window is never re-sent, the server goes on naming
                    // this buffer's handle, and once its slot is reused at a newer
                    // generation the next draw or dispatch that walks the window asks the
                    // backend for the dead one - Espryt dies with Fatal{ProtocolCorruption,
                    // "BackendSlotTable.Generation"} (DeletedBoundBufferScenario).
                    NoteBindPointChanged(BufferBindPointTargets[static_cast<SizeT>(
                        &bindingPointArray - m_bufferBindPointTargets.data())]);
#endif
                }
            }
        }
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
