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
        for (SizeT i = 0; i < m_touchedBindPointCount.size(); ++i) {
            m_touchedBindPointCount[i] = 0;
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
                // Erase through the iterator already in hand: erase(key) would repeat the
                // find() above, and the successor scan that once made key-based
                // erase the cheaper of the two no longer happens here - erase(iterator)
                // hands back an unconverted proxy, and the scan is what converting it
                // would cost. The unbind loops above touch only the binding arrays, so
                // `it` is still live.
                m_bufferObjects.erase(it);
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
        // A GLOBAL TARGET ASKED THROUGH THE INDEXED LOOKUP.  Only four targets have per-index binding
        // points (Uniform, TransformFeedback, AtomicCounter, ShaderStorage); the rest - Vertex, Index,
        // PixelPack/Unpack, Texture, CopyRead/Write, Query, the indirect ones - are global, and a caller
        // that asks for one here is reading the wrong accessor, not passing a bad enum.
        //
        // This used to abort, and the compositor reached it right after its vertex-attrib setup: the
        // whole session died on Trace/breakpoint trap ("Invalid BufferTarget enum value for binding
        // point").  Aborting a compositor over an accessor mix-up is the wrong trade, so the answer is
        // a one-time warning and index 0 of the indexed array: the caller gets a valid slot, the
        // global binding is untouched, and the mismatch stays visible in the log instead of being a
        // crash.  The proper fix is for that caller to use the global accessor.
        MGLOG_W_ONCE("BufferState::GetBindingPoint: target=%d has no per-index binding points; answering "
                     "index 0 of the indexed array (a global target was asked through the indexed lookup)",
                     static_cast<int>(target));
        return m_bufferBindPointTargets[0][index];
    }
} // namespace MobileGL::MG_State::GLState
