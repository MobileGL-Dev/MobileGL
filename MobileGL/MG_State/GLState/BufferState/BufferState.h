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
    // How many indexed binding points each of BufferBindPointTargets gets. 84 is the GL 4.5 core
    // minimum for GL_MAX_UNIFORM_BUFFER_BINDINGS (table 23.64) and this array is the capacity
    // that limit is clamped against - at 36 the clamp in GL_Getter was degenerate (lo == hi) and
    // no application could ever be told about, or bind to, a binding point past the 36th. The
    // other three targets advertise their own, smaller ceilings out of
    // GetIndexedBufferQueryPointCount, so widening this does not widen what they promise; it only
    // costs the unused tail of three arrays.
    constexpr SizeT BufferBindingPointCount = 84;

    class BufferState {
    public:
        BufferState();

        const SharedPtr<BufferObject>& GetBufferObject(Uint index);
        void GenerateNames(Uint number, Vector<Uint>& buffers);
        const SharedPtr<BufferObject>& CreateBufferObject(Uint index);
        BindingSlot<BufferObject>& GetBindingSlot(BufferTarget target);
        // For glBindBufferBase / glBindBufferRange
        BindingSlotRange1D<BufferObject>& GetBindingPoint(BufferTarget target, Uint index);
        const BindingSlotRange1D<BufferObject>& GetBindingPoint(BufferTarget target, Uint index) const {
            return const_cast<BufferState*>(this)->GetBindingPoint(target, index);
        }
        constexpr SizeT GetBindingPointCount(const BufferTarget target) const {
            auto it = std::find(BufferBindPointTargets.begin(), BufferBindPointTargets.end(), target);
            auto index = std::distance(BufferBindPointTargets.begin(), it);
            return m_bufferBindPointTargets[index].size();
        }
        // High-water mark of app-touched binding points per target (highest index + 1, 0 if none).
        // Lets the backend skip syncing the never-touched tail of the fixed 36-point array each draw.
        void TouchBindPoint(const BufferTarget target, Uint index) {
            auto it = std::find(BufferBindPointTargets.begin(), BufferBindPointTargets.end(), target);
            if (it == BufferBindPointTargets.end()) return;
            auto slot = std::distance(BufferBindPointTargets.begin(), it);
            if (static_cast<SizeT>(index) + 1 > m_touchedBindPointCount[slot])
                m_touchedBindPointCount[slot] = static_cast<SizeT>(index) + 1;
        }
        SizeT GetTouchedBindPointCount(const BufferTarget target) const {
            auto it = std::find(BufferBindPointTargets.begin(), BufferBindPointTargets.end(), target);
            if (it == BufferBindPointTargets.end()) return 0;
            return m_touchedBindPointCount[std::distance(BufferBindPointTargets.begin(), it)];
        }
        void MarkBufferObjectForDeletion(Uint index);
        Bool ValidateName(Uint index) const;
        Bool ValidateBufferObject(Uint index) const;

#if MOBILEGL_PIPE_PUSH
    // P2 brief D4: "did the contents of ANY buffer object move". One counter for every
    // BufferObject ++m_changeSerial site, which is what NEW_VERTEX_BUFFERS /
    // NEW_INDEX_BUFFER / NEW_CONST_BUFFERS / NEW_SHADER_BUFFERS / NEW_SO_TARGETS all
    // shutter on in P2 - five bits over one aggregate until P3b splits them.
    void NoteBufferChanged() { ++m_anyBufferChangeGeneration; }
    Uint64 GetAnyBufferChangeGeneration() const { return m_anyBufferChangeGeneration; }

    // P5e (sb, MG_Remote/CONTRACT-P5E.md §1, §5.6). "DID ANY INDEXED BINDING POINT OF THIS
    // TARGET MOVE" - one counter per BufferBindPointTargets entry, and it is the P4b hole
    // closed rather than a new convenience.
    //
    // WHAT WAS WRONG. Dirty bits 15/16/17 shuttered on GetAnyBufferChangeGeneration() - the
    // CONTENT aggregate - while glBindBufferBase / glBindBufferRange mutate a binding point
    // through a returned reference (BindingSlotRange1D::Bind / SetRange), which moves the
    // slot's own Uint16 version and nothing the tracker reads. So
    // `glBindBufferBase(UNIFORM,1,A); draw; glBindBufferBase(UNIFORM,1,B); draw` fired no bit
    // at all: harmless while nothing was emitted for those bits, and an UNDER-FIRE the moment
    // set_shader_buffers is, because the server would go on binding A. It is the same defect
    // class as P4a's glBindSampler hole (c0d) and it is closed the same way - the shutter
    // reads the generation the mutator moves.
    //
    // A COUNTER AND NOT THE SLOT'S OWN VERSION: the slot version is a WRAPPING Uint16 per
    // point, and a shutter over 84 of them would have to observe all 84 through the widened
    // counter every draw. One monotone Uint64 per target answers "could this target's window
    // have moved" in one read, which is what a per-draw shutter can afford.
    //
    // #if MOBILEGL_PIPE_PUSH so the pull build's BufferState does not resize (G1) - the same
    // licence TextureState::NoteImageUnitTouched took at P5d r3.
    void NoteBindPointChanged(const BufferTarget target) {
        auto it = std::find(BufferBindPointTargets.begin(), BufferBindPointTargets.end(), target);
        if (it == BufferBindPointTargets.end()) return;
        ++m_bindPointGeneration[std::distance(BufferBindPointTargets.begin(), it)];
    }
    Uint64 GetBindPointGeneration(const BufferTarget target) const {
        auto it = std::find(BufferBindPointTargets.begin(), BufferBindPointTargets.end(), target);
        if (it == BufferBindPointTargets.end()) return 0;
        return m_bindPointGeneration[std::distance(BufferBindPointTargets.begin(), it)];
    }
#endif

    private:
#if MOBILEGL_PIPE_PUSH
    Uint64 m_anyBufferChangeGeneration = 0;
    Array<Uint64, BufferBindPointTargets.size()> m_bindPointGeneration{};
#endif
        UnorderedMap<Uint, SharedPtr<BufferObject>> m_bufferObjects;
        IndexGenerator<Uint> m_indexGenerator;
        Array<BindingSlot<BufferObject>, GlobalBufferTargets.size()> m_bindingSlots;
        // TODO: query the count somewhere globally?
        // For glBindBufferBase / glBindBufferRange
        Array<Array<BindingSlotRange1D<BufferObject>, BufferBindingPointCount>, BufferBindPointTargets.size()>
            m_bufferBindPointTargets;
        Array<SizeT, BufferBindPointTargets.size()> m_touchedBindPointCount;
    };
} // namespace MobileGL::MG_State::GLState
