// MobileGL - MobileGL/MG_State/GLState/VertexArrayState/VertexArrayObject.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "VertexArrayObject.h"

#include <atomic>

namespace MobileGL::MG_State::GLState {
    // Starts at 1 so a zero-initialized memo slot can never carry a live object's id.
    // Atomic because VAOs are GL-thread-only today but the counter costs nothing to
    // make safe, and a duplicate id would resurrect exactly the bug it exists to kill.
    static std::atomic<Uint64> s_nextVertexArrayLifetimeId{1};

    Uint64 VertexArrayObject::AllocateLifetimeId() {
        return s_nextVertexArrayLifetimeId.fetch_add(1, std::memory_order_relaxed);
    }

    VertexArrayObject::VertexArrayObject(Uint externIndex) : m_externalIndex(externIndex) {
        for (int index = 0; index < MAX_VERTEX_ATTRIBS; ++index) {
            auto& attr = m_attributes[index];
            attr.Enabled = false;
            attr.Size = 4;
            attr.Type = DataType::Float32;
            attr.Normalized = false;
            attr.Stride = 0;
            attr.Offset = 0;
            attr.LegacyStride = 0;
            attr.LegacyPointer = 0;
            attr.Buffer = nullptr;

            BumpAttributeFormatVersion(index);
        }
    }

    void VertexArrayObject::EnableAttribute(Uint index) {
        if (index >= MAX_VERTEX_ATTRIBS) return;

        if (m_attributes[index].Enabled) return;

        m_attributes[index].Enabled = true;
        BumpAttributeSwitchVersion(index);
    }

    void VertexArrayObject::DisableAttribute(Uint index) {
        if (index >= MAX_VERTEX_ATTRIBS) return;

        if (!m_attributes[index].Enabled) return;

        m_attributes[index].Enabled = false;
        BumpAttributeSwitchVersion(index);
    }

    Bool VertexArrayObject::IsAttributeEnabled(Uint index) const {
        if (index >= MAX_VERTEX_ATTRIBS) return false;
        return m_attributes[index].Enabled;
    }

    void VertexArrayObject::SetAttributeFormat(Uint index, int size, DataType type, Bool normalized, int stride,
                                               SizeT offset, Bool isInteger, Bool isBgra) {
        if (index >= MAX_VERTEX_ATTRIBS) return;
        if (size < 1 || size > 4) {
            return;
        }

        // The classic pointer-style API takes back full ownership of the resolved fields.
        m_attributeUsesBindingModel[index] = false;

        // The legacy query shadows: written here and nowhere else, so a later binding-model
        // mutation cannot leak into VERTEX_ATTRIB_ARRAY_STRIDE / _POINTER. They are pure
        // query state, so they carry no version bump of their own.
        m_attributes[index].LegacyStride = stride;
        m_attributes[index].LegacyPointer = offset;

        if (m_attributes[index].Size == size && m_attributes[index].Type == type &&
            m_attributes[index].Normalized == normalized && m_attributes[index].Stride == stride &&
            m_attributes[index].Offset == offset && m_attributes[index].IsInteger == isInteger &&
            m_attributes[index].IsBgra == isBgra && !m_attributes[index].IsLong) {
            return;
        }

        auto& attr = m_attributes[index];
        attr.Size = size;
        attr.Type = type;
        attr.Normalized = normalized;
        attr.Stride = stride;
        attr.Offset = offset;
        attr.IsInteger = isInteger;
        attr.IsBgra = isBgra;
        // glVertexAttribPointer / glVertexAttribIPointer are never the long form, so they always
        // take the attribute back out of it - and "only IsLong changed" is a real change that has to
        // reach the backends, which is why the early-out above tests it too. Cleared inside the
        // mutation block so the clear and the version bump stay atomic.
        attr.IsLong = false;

        BumpAttributeFormatVersion(index);
    }

    void VertexArrayObject::MirrorPointerIntoBinding(Uint index, const SharedPtr<BufferObject>& buffer, SizeT offset,
                                                     int effectiveStride) {
        if (index >= MAX_VERTEX_ATTRIBS || index >= MAX_VERTEX_ATTRIB_BINDINGS) return;

        // glVertexAttribPointer is defined in terms of the binding model (GL 4.6 core 10.3.2): it
        // also sets binding point `index` to the buffer, the pointer as the offset, and the
        // *effective* stride, and points the attribute at that binding point with relative offset 0.
        // The flat attribute view keeps the raw stride, because VERTEX_ATTRIB_ARRAY_STRIDE reports
        // that argument verbatim, so the binding point is recorded alongside the resolved attribute
        // rather than being resolved into it.
        m_attributeBindingIndex[index] = index;
        m_attributeRelativeOffset[index] = 0;

        auto& binding = m_bindingPoints[index];
        binding.Buffer = buffer;
        binding.Offset = offset;
        binding.Stride = effectiveStride;
        binding.Divisor = m_attributes[index].Divisor;

        // Other attributes may already be pointed at this binding point through
        // glVertexAttribBinding; they see the new buffer/offset/stride too (basic-state3
        // checks exactly that after a glVertexAttribPointer). They are not adopted into the
        // binding model here - only the ones already in it re-resolve.
        ResolveAttributesForBinding(index, /*adopt: */ false);
    }

    void VertexArrayObject::BindAttributeBuffer(Uint index, const SharedPtr<BufferObject>& buffer) {
        if (index >= MAX_VERTEX_ATTRIBS) return;

        if (m_attributes[index].Buffer == buffer) return;

        m_attributes[index].Buffer = buffer;
        BumpAttributeBufferVersion(index);
    }

    BindingSlot<BufferObject>& VertexArrayObject::GetIndexBufferBindingSlot() {
        return m_indexBufferBindingSlot;
    }

    const BindingSlot<BufferObject>& VertexArrayObject::GetIndexBufferBindingSlot() const {
        return m_indexBufferBindingSlot;
    }


    const VertexAttribute& VertexArrayObject::GetAttribute(Uint index) const {
        static VertexAttribute emptyAttr;
        if (index >= MAX_VERTEX_ATTRIBS) return emptyAttr;
        return m_attributes[index];
    }

    const Array<VertexAttribute, VertexArrayObject::MAX_VERTEX_ATTRIBS>& VertexArrayObject::GetAllAttributes() const {
        return m_attributes;
    }

    Uint VertexArrayObject::GetExternalIndex() const {
        return m_externalIndex;
    }

    void VertexArrayObject::SetAttributeDivisor(Uint index, Uint divisor) {
        if (index >= MAX_VERTEX_ATTRIBS) return;
        // GL 4.6 core 10.3.2 defines VertexAttribDivisor(i, d) as
        //   VertexAttribBinding(i, i); VertexBindingDivisor(i, d)
        // - the binding is RE-POINTED at i, it is not merely written through when it already
        // happens to be i. Guarding the write on "binding == index" (which is what this did)
        // left an attribute that glVertexAttribBinding had moved elsewhere pointing at the old
        // binding, so the next resolve restored that binding's divisor and the new one was
        // lost (KHR-GL4x.vertex_attrib_binding.basic-state4).
        //
        // What is deliberately NOT copied from VertexAttribBinding is the adoption into the
        // binding model: an attribute configured the classic way keeps its pointer-resolved
        // stride/offset, exactly as before. The binding point mirrors that state already
        // (MirrorPointerIntoBinding), so nothing observable differs - and adopting it here
        // would silently swap the raw pointer stride for the effective one under every
        // application that calls glVertexAttribDivisor after glVertexAttribPointer.
        if (index < MAX_VERTEX_ATTRIB_BINDINGS) {
            m_attributeBindingIndex[index] = index;
            m_bindingPoints[index].Divisor = divisor;
            ResolveAttributesForBinding(index, /*adopt: */ false);
        }
        if (m_attributes[index].Divisor == divisor) return;
        m_attributes[index].Divisor = divisor;
        BumpAttributeFormatVersion(index);
    }

    Uint VertexArrayObject::GetAttributeDivisor(Uint index) const {
        if (index >= MAX_VERTEX_ATTRIBS) return 0;
        return m_attributes[index].Divisor;
    }

    void VertexArrayObject::ResolveAttributeFromBinding(Uint attribIndex) {
        if (attribIndex >= MAX_VERTEX_ATTRIBS) return;

        const Uint bindingIndex = m_attributeBindingIndex[attribIndex];
        if (bindingIndex >= MAX_VERTEX_ATTRIB_BINDINGS) return;
        const auto& binding = m_bindingPoints[bindingIndex];

        auto& attr = m_attributes[attribIndex];

        // VERTEX_ATTRIB_ARRAY_DIVISOR is not independent per-attribute state: it IS the divisor
        // of the binding point the attribute is attached to (GL 4.6 core 10.3.2), whichever API
        // configured the attribute. glVertexBindingDivisor therefore has to reach a classic
        // pointer-configured attribute as well - basic-state4 alternates the two spellings on
        // the same attribute and expects each to win in turn.
        if (attr.Divisor != binding.Divisor) {
            attr.Divisor = binding.Divisor;
            BumpAttributeFormatVersion(attribIndex);
        }

        // Everything else stays owned by whichever API configured the attribute: a classic
        // glVertexAttrib*Pointer attribute keeps its pointer-resolved stride and offset.
        if (!m_attributeUsesBindingModel[attribIndex]) return;

        const SizeT resolvedOffset = binding.Offset + m_attributeRelativeOffset[attribIndex];
        if (attr.Stride != binding.Stride || attr.Offset != resolvedOffset) {
            attr.Stride = binding.Stride;
            attr.Offset = resolvedOffset;
            BumpAttributeFormatVersion(attribIndex);
        }

        if (attr.Buffer != binding.Buffer) {
            attr.Buffer = binding.Buffer;
            BumpAttributeBufferVersion(attribIndex);
        }
    }

    void VertexArrayObject::ResolveAttributesForBinding(Uint bindingIndex, Bool adopt) {
        for (Uint attribIndex = 0; attribIndex < MAX_VERTEX_ATTRIBS; ++attribIndex) {
            if (m_attributeBindingIndex[attribIndex] != bindingIndex) continue;
            if (adopt) m_attributeUsesBindingModel[attribIndex] = true;
            ResolveAttributeFromBinding(attribIndex);
        }
    }

    void VertexArrayObject::SetBindingBuffer(Uint bindingIndex, const SharedPtr<BufferObject>& buffer, SizeT offset,
                                             int stride) {
        if (bindingIndex >= MAX_VERTEX_ATTRIB_BINDINGS) return;

        auto& binding = m_bindingPoints[bindingIndex];
        binding.Buffer = buffer;
        binding.Offset = offset;
        binding.Stride = stride;

        // Binding a vertex buffer to a binding point adopts every attribute currently mapped to
        // that binding point into the binding model (the default mapping is attribute i ->
        // binding i, which matches the GL 4.3 rules for state mixing).
        ResolveAttributesForBinding(bindingIndex, /*adopt: */ true);
    }

    void VertexArrayObject::SetBindingDivisor(Uint bindingIndex, Uint divisor) {
        if (bindingIndex >= MAX_VERTEX_ATTRIB_BINDINGS) return;

        m_bindingPoints[bindingIndex].Divisor = divisor;

        ResolveAttributesForBinding(bindingIndex, /*adopt: */ false);
    }

    void VertexArrayObject::SetAttributeBinding(Uint attribIndex, Uint bindingIndex) {
        if (attribIndex >= MAX_VERTEX_ATTRIBS) return;
        if (bindingIndex >= MAX_VERTEX_ATTRIB_BINDINGS) return;

        m_attributeBindingIndex[attribIndex] = bindingIndex;
        m_attributeUsesBindingModel[attribIndex] = true;
        ResolveAttributeFromBinding(attribIndex);
    }

    void VertexArrayObject::SetAttributeFormatSeparate(Uint attribIndex, int size, DataType type, Bool normalized,
                                                       Bool isInteger, Uint relativeOffset, Bool isBgra,
                                                       Bool isLong) {
        if (attribIndex >= MAX_VERTEX_ATTRIBS) return;
        if (size < 1 || size > 4) return;

        auto& attr = m_attributes[attribIndex];
        if (attr.Size != size || attr.Type != type || attr.Normalized != normalized || attr.IsInteger != isInteger ||
            attr.IsBgra != isBgra || attr.IsLong != isLong ||
            m_attributeRelativeOffset[attribIndex] != relativeOffset) {
            attr.Size = size;
            attr.Type = type;
            attr.Normalized = normalized;
            attr.IsInteger = isInteger;
            attr.IsBgra = isBgra;
            attr.IsLong = isLong;
            m_attributeRelativeOffset[attribIndex] = relativeOffset;
            BumpAttributeFormatVersion(attribIndex);
        }

        m_attributeUsesBindingModel[attribIndex] = true;
        ResolveAttributeFromBinding(attribIndex);
    }

    void VertexArrayObject::BumpAttributeFormatVersion(Uint index) {
        if (index >= MAX_VERTEX_ATTRIBS) return;
        ++m_attributeVersions[index].FormatVersion;
        ++m_configVersion;
    }

    void VertexArrayObject::BumpAttributeBufferVersion(Uint index) {
        if (index >= MAX_VERTEX_ATTRIBS) return;
        ++m_attributeVersions[index].BufferVersion;
        ++m_configVersion;
    }

    void VertexArrayObject::BumpAttributeSwitchVersion(Uint index) {
        if (index >= MAX_VERTEX_ATTRIBS) return;
        ++m_attributeVersions[index].SwitchVersion;
        ++m_configVersion;
    }

    const VertexAttributeVersion& VertexArrayObject::GetAttributeVersion(Uint index) const {
        static VertexAttributeVersion emptyVersion;
        if (index >= MAX_VERTEX_ATTRIBS) return emptyVersion;
        return m_attributeVersions[index];
    }

    const Array<VertexAttributeVersion, VertexArrayObject::MAX_VERTEX_ATTRIBS>& VertexArrayObject::
        GetAllAttributeVersions() const {
        return m_attributeVersions;
    }
} // namespace MobileGL::MG_State::GLState
