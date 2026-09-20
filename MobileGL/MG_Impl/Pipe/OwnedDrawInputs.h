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

            Vector<Fetch> fetches;
            if (indirect != nullptr) {
                if (!ReadIndirectFetches(info.IndexSize, *indirect, fetches)) return false;
            } else {
                fetches.reserve(rangeCount);
                for (Uint32 i = 0; i < rangeCount; ++i)
                    fetches.push_back({ranges[i], info.InstanceCount, info.StartInstance});
            }

            Vector<Uint64> vertices;
            if (needsVertexIndices && !ReadVertexIndices(info, fetches, clientIndices, clientIndexBytes, vertices))
                return false;
            Array<MGPipeHandle, kMGPipeMaxVertexAttribs> handles{};
            for (SizeT location = 0; location < attributes.size(); ++location) {
                const auto& attribute = attributes[location];
                if (!attribute.Enabled || attribute.Buffer) continue;
                const SizeT elementBytes = AttributeBytes(attribute);
                if (!elementBytes || attribute.Stride < 0) return false;
                Vector<Uint64> instances;
                const Vector<Uint64>* referenced = &vertices;
                if (attribute.Divisor != 0) {
                    for (const auto& fetch : fetches) {
                        if (!fetch.Range.Count || !fetch.Instances) continue;
                        const Uint64 first = fetch.BaseInstance;
                        const Uint64 last = first + (fetch.Instances - 1u) / attribute.Divisor;
                        for (Uint64 at = first; at <= last; ++at) instances.push_back(at);
                    }
                    SortUnique(instances);
                    referenced = &instances;
                }
                const Uint64 stride = static_cast<Uint32>(attribute.Stride);
                const Uint64 last = referenced->empty() ? 0 : referenced->back();
                if (stride != 0 && last > (std::numeric_limits<Uint32>::max() - elementBytes) / stride)
                    return false;
                const SizeT byteCount = static_cast<SizeT>(last * stride + elementBytes);
                Vector<Uint8> bytes(byteCount, 0);
                if (!referenced->empty()) {
                    if (attribute.Offset == 0 || byteCount > std::numeric_limits<SizeT>::max() - attribute.Offset)
                        return false;
                    const auto* source = reinterpret_cast<const Uint8*>(attribute.Offset);
                    if (stride == 0) {
                        std::memcpy(bytes.data(), source, elementBytes);
                    } else {
                        // Only dereference fetched elements: sparse indices and first>0
                        // do not grant permission to read intervening application memory.
                        for (const Uint64 vertex : *referenced) {
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
        struct Fetch {
            MGPDrawRange Range;
            Uint32 Instances;
            Uint32 BaseInstance;
        };

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

        static void SortUnique(Vector<Uint64>& values) {
            std::sort(values.begin(), values.end());
            values.erase(std::unique(values.begin(), values.end()), values.end());
        }

        static UniquePtr<Buffer> MakeOwnedBuffer(const void* bytes, SizeT size, BufferTarget target) {
            auto buffer = MakeUnique<Buffer>(0);
            const auto handle = MGPipeResourceTrackerInstance().Find(*buffer);
            MGPipeResourceTrackerInstance().NoteBoundAs(handle, target);
            buffer->SetUsage(BufferUsage::StreamDraw);
            buffer->Respecify(size, bytes);
            return buffer;
        }

        static Bool ReadBuffer(const SharedPtr<Buffer>& buffer, Uint64 offset, SizeT size, void* destination) {
            if (!buffer || offset > buffer->GetSize() || size > buffer->GetSize() - offset) return false;
            // This waits for the actual server writeback when an earlier GPU draw,
            // dispatch, or capture produced the index/command bytes.
            buffer->SyncGpuWrites();
            buffer->DownloadSubData(destination, static_cast<SizeT>(offset), size);
            return true;
        }

        Bool ReadIndirectFetches(Uint8 indexSize, const MGPDrawIndirect& indirect, Vector<Fetch>& fetches) {
            const auto& commands = m_context.GetBufferBindingSlot(BufferTarget::DrawIndirect).GetBoundObject();
            Uint32 count = indirect.DrawCount;
            if (!MGPipeHandleIsNull(indirect.ParameterBuffer)) {
                const auto& parameter = m_context.GetBufferBindingSlot(BufferTarget::Parameter).GetBoundObject();
                Uint32 actual = 0;
                if (!ReadBuffer(parameter, indirect.ParameterOffset, sizeof(actual), &actual)) return false;
                count = std::min(count, actual);
            }
            const Uint64 commandBytes = indexSize ? 5 * sizeof(Uint32) : 4 * sizeof(Uint32);
            const Uint64 stride = indirect.Stride ? indirect.Stride : commandBytes;
            fetches.reserve(count);
            for (Uint32 i = 0; i < count; ++i) {
                const Uint64 displacement = static_cast<Uint64>(i) * stride;
                if (displacement > std::numeric_limits<Uint64>::max() - indirect.Offset) return false;
                Uint32 words[5]{};
                if (!ReadBuffer(commands, indirect.Offset + displacement, commandBytes, words)) return false;
                Int32 bias = 0;
                if (indexSize) std::memcpy(&bias, &words[3], sizeof(bias));
                fetches.push_back({{words[2], words[0], bias}, words[1], words[indexSize ? 4 : 3]});
            }
            return true;
        }

        Bool ReadVertexIndices(const MGPDrawInfo& info, const Vector<Fetch>& fetches,
                               const void* clientIndices, Uint64 clientIndexBytes, Vector<Uint64>& vertices) {
            const auto& vao = m_context.GetBoundVertexArray();
            const auto& elementBuffer = vao->GetIndexBufferBindingSlot().GetBoundObject();
            const Bool restart = (info.Flags & kDrawPrimitiveRestart) != 0;
            const Bool fixedRestart = m_context.IsCapabilityEnabled(CapabilityInput::PrimitiveRestartFixedIndex);
            const Uint32 restartIndex = fixedRestart
                ? (info.IndexSize == 1 ? 0xffu : info.IndexSize == 2 ? 0xffffu : 0xffffffffu)
                : info.RestartIndex;
            for (const auto& fetch : fetches) {
                if (!fetch.Range.Count || !fetch.Instances) continue;
                if (!info.IndexSize) {
                    for (Uint64 n = 0; n < fetch.Range.Count; ++n)
                        vertices.push_back(static_cast<Uint64>(fetch.Range.Start) + n);
                    continue;
                }
                const Uint64 offset = static_cast<Uint64>(fetch.Range.Start) * info.IndexSize;
                const Uint64 size = static_cast<Uint64>(fetch.Range.Count) * info.IndexSize;
                if (size > std::numeric_limits<SizeT>::max()) return false;
                Vector<Uint8> indexBytes;
                const Uint8* data = nullptr;
                if (clientIndices != nullptr) {
                    if (offset > clientIndexBytes || size > clientIndexBytes - offset) return false;
                    data = static_cast<const Uint8*>(clientIndices) + static_cast<SizeT>(offset);
                } else {
                    indexBytes.resize(static_cast<SizeT>(size));
                    if (!ReadBuffer(elementBuffer, offset, indexBytes.size(), indexBytes.data())) return false;
                    data = indexBytes.data();
                }
                for (Uint32 n = 0; n < fetch.Range.Count; ++n) {
                    Uint32 index = 0;
                    std::memcpy(&index, data + static_cast<SizeT>(n) * info.IndexSize, info.IndexSize);
                    if (restart && index == restartIndex) continue;
                    const Int64 vertex = static_cast<Int64>(index) + fetch.Range.IndexBias;
                    if (vertex < 0) return false;
                    vertices.push_back(static_cast<Uint64>(vertex));
                }
            }
            SortUnique(vertices);
            return true;
        }

        Context& m_context;
        Array<UniquePtr<Buffer>, kMGPipeMaxVertexAttribs> m_vertexBuffers{};
        UniquePtr<Buffer> m_indexBuffer;
        Uint32 m_baseInstance = 0;
        Bool m_vertexBindingsChanged = false;
        Bool m_indexBindingChanged = false;
    };
} // namespace MobileGL::MG_Pipe
#endif
