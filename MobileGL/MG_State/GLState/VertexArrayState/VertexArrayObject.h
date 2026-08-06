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
                // GL_BGRA vertex size: four components in reversed (B,G,R,A) memory order. Size stays 4.
                // Set only by the long (L) format entry points. It is NOT implied by
                // Type == Float64: VertexAttribFormat(GL_DOUBLE) also reads doubles from memory but
                // asks for them *converted to float*, while VertexAttribLFormat keeps all 64 bits
                // (GL 4.6 core 10.3.2). Backends have to tell the two apart, and it is what
                // GL_VERTEX_ATTRIB_ARRAY_LONG reports.
                Bool IsLong = false;
                Bool IsBgra = false;
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
                                        Bool isInteger, Bool isBgra = false);

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

                // Backend-owned resolved-state memo: an opaque pointer into the
                // backend's vertex-input-state cache plus the cache's eviction
                // epoch, valid while the config version matches. Lets the
                // per-draw path skip the content hash AND the cache lookup; the
                // epoch guards against the cache evicting the pointee.
                Bool GetBackendStateMemo(const void*& outState, Uint64& outEpoch) const {
                    if (m_backendStateMemoVersion != m_configVersion) return false;
                    outState = m_backendStateMemo;
                    outEpoch = m_backendStateMemoEpoch;
                    return true;
                }
                void SetBackendStateMemo(const void* state, Uint64 epoch) const {
                    m_backendStateMemo = state;
                    m_backendStateMemoEpoch = epoch;
                    m_backendStateMemoVersion = m_configVersion;
                }

                // Backend-owned aux memo: two opaque VALUE words (no pointee, so unlike the
                // state memo above they need no eviction-epoch guard), valid while the config
                // version matches. They live next to m_configVersion, which every per-draw
                // path already loads, so a backend can re-read small derived facts about this
                // VAO's configuration (e.g. a layout hash and attribute masks) without
                // chasing into its own cache's heap entry - that chase is a guaranteed cache
                // miss when an app cycles hundreds of VAOs per frame.
                Bool GetBackendAuxMemo(Uint64& outAux0, Uint64& outAux1) const {
                    if (m_backendAuxMemoVersion != m_configVersion) return false;
                    outAux0 = m_backendAuxMemo0;
                    outAux1 = m_backendAuxMemo1;
                    return true;
                }
                void SetBackendAuxMemo(Uint64 aux0, Uint64 aux1) const {
                    m_backendAuxMemo0 = aux0;
                    m_backendAuxMemo1 = aux1;
                    m_backendAuxMemoVersion = m_configVersion;
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
                mutable const void* m_backendStateMemo = nullptr;
                mutable Uint64 m_backendStateMemoEpoch = 0;
                mutable Uint32 m_backendStateMemoVersion = ~0u;
                mutable Uint64 m_backendAuxMemo0 = 0;
                mutable Uint64 m_backendAuxMemo1 = 0;
                mutable Uint32 m_backendAuxMemoVersion = ~0u;
            };
        } // namespace GLState
    } // namespace MG_State
} // namespace MobileGL
