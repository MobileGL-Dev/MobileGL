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

namespace MobileGL {
    namespace MG_State {
        namespace GLState {
            struct VertexAttribute {
                Bool Enabled = false;
                int Size = 4;
                DataType Type = DataType::Float32;
                Bool Normalized = false;
                int Stride = 0;
                SizeT Offset = 0;
                Bool IsInteger = false;
                Uint Divisor = 0;
                SharedPtr<BufferObject> Buffer;
            };

            // ARB_vertex_attrib_binding separate binding point. Attributes configured through the
            // binding-point API are resolved eagerly into the flat VertexAttribute view above, so
            // backends keep consuming resolved attributes and never see binding points.
            struct VertexBufferBindingPoint {
                SharedPtr<BufferObject> Buffer;
                SizeT Offset = 0;
                int Stride = 0;
                Uint Divisor = 0;
            };

            struct VertexAttributeVersion {
                Uint16 FormatVersion = 0;
                Uint16 BufferVersion = 0;
                Uint16 SwitchVersion = 0;
            };

            class VertexArrayObject {
            public:
                static constexpr int MAX_VERTEX_ATTRIBS = 16;
                static constexpr int MAX_VERTEX_ATTRIB_BINDINGS = 16;

                VertexArrayObject(Uint externIndex);

                void EnableAttribute(Uint index);
                void DisableAttribute(Uint index);
                Bool IsAttributeEnabled(Uint index) const;

                void SetAttributeFormat(Uint index, int size, DataType type, Bool normalized, int stride, SizeT offset,
                                        Bool isInteger);

                void BindAttributeBuffer(Uint index, const SharedPtr<BufferObject>& buffer);

                BindingSlot<BufferObject>& GetIndexBufferBindingSlot();
                const BindingSlot<BufferObject>& GetIndexBufferBindingSlot() const;

                const VertexAttribute& GetAttribute(Uint index) const;
                const Array<VertexAttribute, MAX_VERTEX_ATTRIBS>& GetAllAttributes() const;

                Uint GetExternalIndex() const;

                void SetAttributeDivisor(Uint index, Uint divisor);
                Uint GetAttributeDivisor(Uint index) const;

                // ARB_vertex_attrib_binding style state. Each mutation re-resolves the affected
                // attributes into the flat VertexAttribute view.
                void SetBindingBuffer(Uint bindingIndex, const SharedPtr<BufferObject>& buffer, SizeT offset,
                                      int stride);
                void SetBindingDivisor(Uint bindingIndex, Uint divisor);
                void SetAttributeBinding(Uint attribIndex, Uint bindingIndex);
                void SetAttributeFormatSeparate(Uint attribIndex, int size, DataType type, Bool normalized,
                                                Bool isInteger, Uint relativeOffset);

                const VertexAttributeVersion& GetAttributeVersion(Uint index) const;
                const Array<VertexAttributeVersion, MAX_VERTEX_ATTRIBS>& GetAllAttributeVersions() const;

            private:
                void BumpAttributeFormatVersion(Uint index);
                void BumpAttributeBufferVersion(Uint index);
                void BumpAttributeSwitchVersion(Uint index);
                void ResolveAttributeFromBinding(Uint attribIndex);

                const Uint m_externalIndex = 0;
                Array<VertexAttribute, MAX_VERTEX_ATTRIBS> m_attributes;
                Array<VertexAttributeVersion, MAX_VERTEX_ATTRIBS> m_attributeVersions;
                BindingSlot<BufferObject> m_indexBufferBindingSlot;

                Array<VertexBufferBindingPoint, MAX_VERTEX_ATTRIB_BINDINGS> m_bindingPoints;
                Array<Uint, MAX_VERTEX_ATTRIBS> m_attributeBindingIndex = {0,  1,  2,  3,  4,  5,  6,  7,
                                                                           8,  9,  10, 11, 12, 13, 14, 15};
                Array<Uint, MAX_VERTEX_ATTRIBS> m_attributeRelativeOffset = {};
                // Set once an attribute (or its binding point) is touched through the
                // ARB_vertex_attrib_binding API; only such attributes are re-resolved, so the
                // classic glVertexAttribPointer path keeps its exact historical behavior.
                Array<Bool, MAX_VERTEX_ATTRIBS> m_attributeUsesBindingModel = {};
            };
        } // namespace GLState
    } // namespace MG_State
} // namespace MobileGL
