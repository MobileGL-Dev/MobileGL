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
                // Storage capacity, not the GL-visible limit. GL_MAX_VERTEX_ATTRIBS is reported as
                // min(backend limit, MAX_VERTEX_ATTRIBS) and validated against that dynamic value;
                // 32 is the width of the Uint32 attribute masks the backends pass around, so it is
                // also the hard ceiling.
                static constexpr int MAX_VERTEX_ATTRIBS = 32;
                static constexpr int MAX_VERTEX_ATTRIB_BINDINGS = 32;

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

                // Aggregate of every per-attribute version bump; lets backends detect
                // "any vertex-input state changed" with one compare.
                Uint32 GetConfigVersion() const { return m_configVersion; }

                // Backend-owned content-hash memo, valid while the config version matches
                // (same idea as ProgramObject's hash memo — avoids re-hashing all
                // attributes on every draw).
                Bool GetBackendHashMemo(Uint64& outHash) const {
                    if (m_backendHashMemoVersion != m_configVersion) return false;
                    outHash = m_backendHashMemo;
                    return true;
                }
                void SetBackendHashMemo(Uint64 hash) const {
                    m_backendHashMemo = hash;
                    m_backendHashMemoVersion = m_configVersion;
                }

            private:
                void BumpAttributeFormatVersion(Uint index);
                void BumpAttributeBufferVersion(Uint index);
                void BumpAttributeSwitchVersion(Uint index);
                void ResolveAttributeFromBinding(Uint attribIndex);

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

                const Uint m_externalIndex = 0;
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
                mutable Uint64 m_backendHashMemo = 0;
                mutable Uint32 m_backendHashMemoVersion = ~0u;
            };
        } // namespace GLState
    } // namespace MG_State
} // namespace MobileGL
