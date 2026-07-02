// MobileGL - MobileGL/MG_State/GLState/BufferState/BufferState.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_Util/Miscellany/IndexGenerator.h>
#include "BufferObject.h"

namespace MobileGL::MG_State::GLState {
    constexpr const auto GlobalBufferTargets =
        ToArray(BufferTarget::Vertex, BufferTarget::Uniform, BufferTarget::CopyRead, BufferTarget::CopyWrite,
                BufferTarget::PixelPack, BufferTarget::PixelUnpack, BufferTarget::Query, BufferTarget::Texture,
                BufferTarget::TransformFeedback, BufferTarget::AtomicCounter, BufferTarget::DispatchIndirect,
                BufferTarget::DrawIndirect, BufferTarget::Parameter, BufferTarget::ShaderStorage);
    constexpr const auto BufferBindPointTargets = ToArray(BufferTarget::Uniform, BufferTarget::TransformFeedback,
                                                          BufferTarget::AtomicCounter, BufferTarget::ShaderStorage);
    constexpr SizeT BufferBindingPointCount = 36;

    class BufferState {
    public:
        BufferState();

        const SharedPtr<BufferObject>& GetBufferObject(Uint index);
        void GenerateNames(Uint number, Vector<Uint>& buffers);
        const SharedPtr<BufferObject>& CreateBufferObject(Uint index);
        BindingSlot<BufferObject>& GetBindingSlot(BufferTarget target);
        // For glBindBufferBase / glBindBufferRange
        BindingSlotRange1D<BufferObject>& GetBindingPoint(BufferTarget target, Uint index);
        constexpr SizeT GetBindingPointCount(const BufferTarget target) const {
            auto it = std::find(BufferBindPointTargets.begin(), BufferBindPointTargets.end(), target);
            auto index = std::distance(BufferBindPointTargets.begin(), it);
            return m_bufferBindPointTargets[index].size();
        }
        void MarkBufferObjectForDeletion(Uint index);
        Bool ValidateName(Uint index) const;
        Bool ValidateBufferObject(Uint index) const;

    private:
        UnorderedMap<Uint, SharedPtr<BufferObject>> m_bufferObjects;
        IndexGenerator<Uint> m_indexGenerator;
        Array<BindingSlot<BufferObject>, GlobalBufferTargets.size()> m_bindingSlots;
        // TODO: query the count somewhere globally?
        // For glBindBufferBase / glBindBufferRange
        Array<Array<BindingSlotRange1D<BufferObject>, BufferBindingPointCount>, BufferBindPointTargets.size()>
            m_bufferBindPointTargets;
    };
} // namespace MobileGL::MG_State::GLState
