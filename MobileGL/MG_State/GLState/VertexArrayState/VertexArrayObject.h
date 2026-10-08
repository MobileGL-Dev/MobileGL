// MobileGL - MobileGL/MG_State/GLState/VertexArrayState/VertexArrayObject.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include "../BufferState/BufferObject.h"
#include "MG_Util/Types.h"
#include <MG_Pipe/MGPipeValueTypes.h>

namespace MobileGL {
    namespace MG_State {
        namespace GLState {
            class VertexArrayObject {
            public:
                // Storage capacity, not the GL-visible limit. GL_MAX_VERTEX_ATTRIBS is reported as
                // min(backend limit, MAX_VERTEX_ATTRIBS) and validated against that dynamic value;
                // 32 is the width of the Uint32 attribute masks the backends pass around, so it is
                // also the hard ceiling.
                static constexpr int MAX_VERTEX_ATTRIBS = 32;
                static constexpr int MAX_VERTEX_ATTRIB_BINDINGS = 32;

                VertexArrayObject(Uint externIndex);
                // P2 step e2. Out of line, and declared only where there is a notice to raise:
                // in a pull build this class keeps its implicit destructor, which is what keeps
                // the pull build's symbol set byte-for-byte the pre-P2 one (G1).
                ~VertexArrayObject();

                void EnableAttribute(Uint index);
                void DisableAttribute(Uint index);
                Bool IsAttributeEnabled(Uint index) const;

                // `stride` is the raw glVertexAttrib*Pointer argument, reported verbatim by
                // GL_VERTEX_ATTRIB_ARRAY_STRIDE. `effectiveStride` is what the fetch actually
                // advances by - the same value when the argument is non-zero, the tightly
                // packed element size when it is zero. Pass -1 to say the two are the same.
                void SetAttributeFormat(Uint index, int size, DataType type, Bool normalized, int stride, SizeT offset,
                                        Bool isInteger, Bool isBgra = false, int effectiveStride = -1);

                void BindAttributeBuffer(Uint index, const SharedPtr<BufferObject>& buffer);

                // Record what the pointer-style API implies for the binding-point view: attribute
                // `index` bound to binding point `index` with relative offset 0, and that binding
                // point carrying the buffer, the pointer offset and the effective stride.
                void MirrorPointerIntoBinding(Uint index, const SharedPtr<BufferObject>& buffer, SizeT offset,
                                              int effectiveStride);

                BindingSlot<BufferObject>& GetIndexBufferBindingSlot();
                const BindingSlot<BufferObject>& GetIndexBufferBindingSlot() const;

                const VertexAttribute& GetAttribute(Uint index) const;
                const Array<VertexAttribute, MAX_VERTEX_ATTRIBS>& GetAllAttributes() const;

                Uint GetExternalIndex() const;

                // Globally-unique, never-reused id for THIS object's lifetime - the same
                // contract as ProgramObject::GetLifetimeId(), and needed for the same
                // reason. Neither the GL name (freed to a LIFO list and handed straight
                // back by the next glGenVertexArrays) nor the heap address (freed to the
                // allocator and handed straight back by the next allocation of this size)
                // can tell a deleted-and-recreated VAO from the original, so a backend
                // memo keyed on either one silently inherits the dead object's contents.
                // That is not hypothetical: it is what let a transform-feedback capture
                // fetch a destroyed VAO's vertex buffer slice (see the VaoDrawMemo key in
                // DirectVulkan's VulkanRenderer).
                Uint64 GetLifetimeId() const { return m_lifetimeId; }

                void SetAttributeDivisor(Uint index, Uint divisor);
                Uint GetAttributeDivisor(Uint index) const;

                // ARB_vertex_attrib_binding style state. Each mutation re-resolves the affected
                // attributes into the flat VertexAttribute view.
                void SetBindingBuffer(Uint bindingIndex, const SharedPtr<BufferObject>& buffer, SizeT offset,
                                      int stride);
                void SetBindingDivisor(Uint bindingIndex, Uint divisor);
                void SetAttributeBinding(Uint attribIndex, Uint bindingIndex);
                void SetAttributeFormatSeparate(Uint attribIndex, int size, DataType type, Bool normalized,
                                                Bool isInteger, Uint relativeOffset, Bool isBgra = false,
                                                Bool isLong = false);

                // The binding-point view the attributes were resolved from. Kept queryable
                // because glGetVertexArrayIndexed[64]iv reports it verbatim, and the resolved
                // flat attribute cannot always be inverted back into it.
                Uint GetAttributeRelativeOffset(Uint attribIndex) const {
                    return attribIndex < m_attributeRelativeOffset.size() ? m_attributeRelativeOffset[attribIndex] : 0;
                }
                Uint GetAttributeBindingIndex(Uint attribIndex) const {
                    return attribIndex < m_attributeBindingIndex.size() ? m_attributeBindingIndex[attribIndex]
                                                                       : attribIndex;
                }
                const VertexBufferBindingPoint& GetBindingPoint(Uint bindingIndex) const {
                    static const VertexBufferBindingPoint kEmpty{};
                    return bindingIndex < m_bindingPoints.size() ? m_bindingPoints[bindingIndex] : kEmpty;
                }

                const VertexAttributeVersion& GetAttributeVersion(Uint index) const;
                const Array<VertexAttributeVersion, MAX_VERTEX_ATTRIBS>& GetAllAttributeVersions() const;

                // Aggregate of every per-attribute version bump; lets backends detect
                // "any vertex-input state changed" with one compare.
                Uint32 GetConfigVersion() const { return m_configVersion; }
                // P15: moves with every bump EXCEPT a buffer change that keeps the attribute's
                // buffer present (or absent) - a glBindVertexBuffer that only swaps the buffer.
                // That is everything a vertex-elements record (formats, offsets, strides,
                // enables, divisors) can see, so the record's emitter keys on it.
                Uint32 GetElementsVersion() const { return m_elementsVersion; }


            private:
                void BumpAttributeFormatVersion(Uint index);
                void BumpAttributeBufferVersion(Uint index, Bool presenceChanged);
                void BumpAttributeSwitchVersion(Uint index);
                void ResolveAttributeFromBinding(Uint attribIndex);
                // Re-resolve every attribute currently pointed at `bindingIndex`. `adopt` turns
                // the ones that are not in the binding model yet into binding-model attributes
                // first (what glBindVertexBuffer does, GL 4.3 rules for state mixing).
                void ResolveAttributesForBinding(Uint bindingIndex, Bool adopt);

                // The default mapping is attribute i -> binding point i. Keep it an iota over
                // MAX_VERTEX_ATTRIBS rather than a literal list: a literal list silently leaves the
                // tail mapped to binding point 0 whenever the limit grows.
                static constexpr Array<Uint, MAX_VERTEX_ATTRIBS> MakeIdentityAttributeBindings() {
                    Array<Uint, MAX_VERTEX_ATTRIBS> mapping{};
                    for (Uint index = 0; index < static_cast<Uint>(MAX_VERTEX_ATTRIBS); ++index) {
                        mapping[index] = index;
                    }
                    return mapping;
                }

                static Uint64 AllocateLifetimeId();

                const Uint m_externalIndex = 0;
                const Uint64 m_lifetimeId = AllocateLifetimeId();
                Array<VertexAttribute, MAX_VERTEX_ATTRIBS> m_attributes;
                Array<VertexAttributeVersion, MAX_VERTEX_ATTRIBS> m_attributeVersions;
                BindingSlot<BufferObject> m_indexBufferBindingSlot;

                Array<VertexBufferBindingPoint, MAX_VERTEX_ATTRIB_BINDINGS> m_bindingPoints;
                Array<Uint, MAX_VERTEX_ATTRIBS> m_attributeBindingIndex = MakeIdentityAttributeBindings();
                Array<Uint, MAX_VERTEX_ATTRIBS> m_attributeRelativeOffset = {};
                // Set once an attribute (or its binding point) is touched through the
                // ARB_vertex_attrib_binding API; only such attributes are re-resolved, so the
                // classic glVertexAttribPointer path keeps its exact historical behavior.
                Array<Bool, MAX_VERTEX_ATTRIBS> m_attributeUsesBindingModel = {};

                Uint32 m_configVersion = 0;
                Uint32 m_elementsVersion = 0;
            };
        } // namespace GLState
    } // namespace MG_State
} // namespace MobileGL
