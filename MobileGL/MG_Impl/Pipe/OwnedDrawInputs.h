// MobileGL - MobileGL/MG_Impl/Pipe/OwnedDrawInputs.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Client memory becomes ordinary owned buffer resources before a draw.
#pragma once

#if MOBILEGL_BUILD_DISAGGREGATED && MOBILEGL_PIPE_PUSH
#include <MG_Impl/Pipe/ClientFetchPlan.h>
#include <MG_Impl/Pipe/VertexInputEmit.h>
#include <MG_State/GLState/BufferState/BufferObject.h>
#include <algorithm>
#include <cstring>
#include <limits>

namespace MobileGL::MG_Pipe {

    // These private buffers have no GL name or frontend binding. Their normal
    // resource records own the bytes on the server, and normal resource retirement
    // keeps an already submitted draw alive after this client-side scope ends.
    // Restore the application's wire bindings before destroying the private handles.
    class MGPipeOwnedDrawInputs {
    public:
        using Buffer = MG_State::GLState::BufferObject;
        using Context = MG_State::GLState::GLContext;

        explicit MGPipeOwnedDrawInputs(Context& context) : m_context(context) {}
        ~MGPipeOwnedDrawInputs() {
            if (m_vertexBindingsChanged)
                MGPipeVertexInputEmitterInstance().EmitVertexBuffers(m_context, m_baseInstance);
            if (m_indexBindingChanged)
                MGPipeVertexInputEmitterInstance().EmitIndexBuffer(m_context);
        }

        Bool Prepare(MGPDrawInfo& info, const MGPDrawRange* ranges, Uint32 rangeCount,
                     const void* clientIndices, Uint64 clientIndexBytes, const MGPDrawIndirect* indirect) {
            m_baseInstance = info.StartInstance;
            const auto& vao = m_context.GetBoundVertexArray();
            if (!vao) return (info.Flags & kDrawClientArrays) == 0 && clientIndexBytes == 0;

            // A single owned EBO also represents flattened MultiDraw client indices.
            // Start remains an element offset, so base vertex and draw IDs are unchanged.
            if (clientIndexBytes != 0) {
                if (!clientIndices || clientIndexBytes > std::numeric_limits<Uint32>::max()) return false;
                m_indexBuffer = MakeOwnedBuffer(clientIndices, static_cast<SizeT>(clientIndexBytes), BufferTarget::Index);
                info.IndexResource = MGPipeResourceTrackerInstance().Find(*m_indexBuffer);
                info.Flags &= ~static_cast<Uint8>(kDrawHasUserIndices);
                MGPIndexBuffer binding{};
                binding.Res = info.IndexResource;
                binding.IndexSize = info.IndexSize;
                MGPipeRouteSetIndexBuffer(binding);
                m_indexBindingChanged = true;
            }

            if ((info.Flags & kDrawClientArrays) == 0) return true;
            const auto& attributes = vao->GetAllAttributes();
            Bool needsVertexIndices = false;
            for (const auto& attribute : attributes)
                needsVertexIndices |= attribute.Enabled && !attribute.Buffer && attribute.Divisor == 0;

            // The element, command and parameter buffers are read through one reader for
            // WHICH elements this draw fetches, and that question lives in
            // ClientFetchPlan.h - the monolith arm asks the same one of the same bytes.
            const SharedPtr<Buffer> elementBuffer = vao->GetIndexBufferBindingSlot().GetBoundObject();
            const SharedPtr<Buffer> commandBuffer =
                m_context.GetBufferBindingSlot(BufferTarget::DrawIndirect).GetBoundObject();
            const SharedPtr<Buffer> parameterBuffer =
                m_context.GetBufferBindingSlot(BufferTarget::Parameter).GetBoundObject();
            ByteSources sources{&elementBuffer, &commandBuffer, &parameterBuffer};

            MGPipeClientDrawInputs inputs{};
            inputs.IndexSize = info.IndexSize;
            inputs.Ranges = ranges;
            inputs.RangeCount = rangeCount;
            inputs.InstanceCount = info.InstanceCount;
            inputs.BaseInstance = info.StartInstance;
            inputs.PrimitiveRestart = (info.Flags & kDrawPrimitiveRestart) != 0;
            const Bool fixedRestart = m_context.IsCapabilityEnabled(CapabilityInput::PrimitiveRestartFixedIndex);
            inputs.RestartIndex = fixedRestart
                ? (info.IndexSize == 1 ? 0xffu : info.IndexSize == 2 ? 0xffffu : 0xffffffffu)
                : info.RestartIndex;
            inputs.ClientIndices = clientIndices;
            inputs.ClientIndexBytes = clientIndexBytes;
            inputs.WantVertices = needsVertexIndices;
            MGPipeClientIndirect indirectInputs{};
            if (indirect != nullptr) {
                indirectInputs.Offset = indirect->Offset;
                indirectInputs.ParameterOffset = indirect->ParameterOffset;
                indirectInputs.Stride = indirect->Stride;
                indirectInputs.DrawCount = indirect->DrawCount;
                indirectInputs.HasParameterBuffer = !MGPipeHandleIsNull(indirect->ParameterBuffer);
                inputs.Indirect = &indirectInputs;
            }
            if (!m_plan.Build(inputs, &ReadBytes, &sources)) return false;

            Array<MGPipeHandle, kMGPipeMaxVertexAttribs> handles{};
            Vector<Uint64> referenced;
            for (SizeT location = 0; location < attributes.size(); ++location) {
                const auto& attribute = attributes[location];
                if (!attribute.Enabled || attribute.Buffer) continue;
                const SizeT elementBytes = AttributeBytes(attribute);
                if (!elementBytes || attribute.Stride < 0) return false;
                if (!m_plan.Elements(static_cast<Uint32>(attribute.Divisor), referenced)) return false;
                const Uint64 stride = static_cast<Uint32>(attribute.Stride);
                const Uint64 last = referenced.empty() ? 0 : referenced.back();
                if (stride != 0 && last > (std::numeric_limits<Uint32>::max() - elementBytes) / stride)
                    return false;
                const SizeT byteCount = static_cast<SizeT>(last * stride + elementBytes);
                Vector<Uint8> bytes(byteCount, 0);
                if (!referenced.empty()) {
                    if (attribute.Offset == 0 || byteCount > std::numeric_limits<SizeT>::max() - attribute.Offset)
                        return false;
                    const auto* source = reinterpret_cast<const Uint8*>(attribute.Offset);
                    if (stride == 0) {
                        std::memcpy(bytes.data(), source, elementBytes);
                    } else {
                        // Only dereference fetched elements: sparse indices and first>0
                        // do not grant permission to read intervening application memory.
                        for (const Uint64 vertex : referenced) {
                            const SizeT offset = static_cast<SizeT>(vertex * stride);
                            std::memcpy(bytes.data() + offset, source + offset, elementBytes);
                        }
                    }
                }
                m_vertexBuffers[location] = MakeOwnedBuffer(bytes.data(), bytes.size(), BufferTarget::Vertex);
                handles[location] = MGPipeResourceTrackerInstance().Find(*m_vertexBuffers[location]);
            }
            MGPipeVertexInputEmitterInstance().EmitVertexBuffers(m_context, info.StartInstance, &handles);
            m_vertexBindingsChanged = true;
            info.Flags &= ~static_cast<Uint8>(kDrawClientArrays);
            return true;
        }

    private:
        // The three buffers one draw can fetch bytes out of, and the reader that resolves
        // them. `user` is a ByteSources owned by the caller's scope, so nothing here outlives
        // the Prepare that built it.
        struct ByteSources {
            const SharedPtr<Buffer>* Elements = nullptr;
            const SharedPtr<Buffer>* Commands = nullptr;
            const SharedPtr<Buffer>* Parameters = nullptr;
        };

        static Bool ReadBytes(void* user, MGPipeClientBufferKind kind, Uint64 offset, SizeT size, void* destination) {
            const auto& sources = *static_cast<const ByteSources*>(user);
            switch (kind) {
            case MGPipeClientBufferKind::Element:
                return MGPipeReadBufferShadow(*sources.Elements, offset, size, destination);
            case MGPipeClientBufferKind::IndirectCommand:
                return MGPipeReadBufferShadow(*sources.Commands, offset, size, destination);
            case MGPipeClientBufferKind::IndirectCount:
                return MGPipeReadBufferShadow(*sources.Parameters, offset, size, destination);
            }
            return false;
        }

        static SizeT AttributeBytes(const MG_State::GLState::VertexAttribute& attribute) {
            if (attribute.IsBgra || attribute.Type == DataType::Int2101010Rev ||
                attribute.Type == DataType::Uint2101010Rev) return 4;
            SizeT component = 0;
            switch (attribute.Type) {
            case DataType::Int8: case DataType::Uint8: component = 1; break;
            case DataType::Int16: case DataType::Uint16: case DataType::Float16: component = 2; break;
            case DataType::Int32: case DataType::Uint32: case DataType::Float32: case DataType::Fixed32:
                component = 4; break;
            case DataType::Float64: component = 8; break;
            default: return 0;
            }
            return attribute.Size > 0 && attribute.Size <= 4 ? component * attribute.Size : 0;
        }

        static UniquePtr<Buffer> MakeOwnedBuffer(const void* bytes, SizeT size, BufferTarget target) {
            auto buffer = MakeUnique<Buffer>(0);
            const auto handle = MGPipeResourceTrackerInstance().Find(*buffer);
            MGPipeResourceTrackerInstance().NoteBoundAs(handle, target);
            buffer->SetUsage(BufferUsage::StreamDraw);
            buffer->Respecify(size, bytes);
            return buffer;
        }

        Context& m_context;
        MGPipeClientFetchPlan m_plan;
        Array<UniquePtr<Buffer>, kMGPipeMaxVertexAttribs> m_vertexBuffers{};
        UniquePtr<Buffer> m_indexBuffer;
        Uint32 m_baseInstance = 0;
        Bool m_vertexBindingsChanged = false;
        Bool m_indexBindingChanged = false;
    };
} // namespace MobileGL::MG_Pipe
#endif
