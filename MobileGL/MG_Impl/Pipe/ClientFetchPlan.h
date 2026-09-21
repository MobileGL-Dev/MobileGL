// MobileGL - MobileGL/MG_Impl/Pipe/ClientFetchPlan.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// WHICH ELEMENTS OF A CLIENT-MEMORY ARRAY A DRAW ACTUALLY FETCHES.
//
// A client-memory vertex array has no extent the object could report, so "how much of it
// has to exist before this draw can run" is a question only the DRAW can answer: the
// index bytes it will read, the instance count and base instance it shifts by, and - for
// the indirect forms - the command block a shader may have written moments earlier.
//
// Two roles ask it and for the same reason. The wire arm asks it to snapshot the fetched
// elements into owned buffers before the draw crosses (MGPipeOwnedDrawInputs), and the
// monolith arm asks it to size the per-draw upload the backend has always done from the
// application's own pointer (DirectGLES' SyncClientSideAttributesForDraw*, Magma's
// UploadAndBindVertexBuffers). The question is identical, so the answer lives here once:
// two copies of this arithmetic would be two chances to disagree about which vertices a
// draw reads, and disagreeing is a wrong picture rather than an error.
//
// WHAT IT DELIBERATELY DOES NOT DO: dereference application memory. The plan names element
// INDICES; reading the bytes behind them is the caller's, because only the caller knows
// whether the elements are read from the client's own array or out of a store the server
// owns.
//
// COMPILED IN EVERY FLAVOR, and the guard it used to carry (`MOBILEGL_BUILD_DISAGGREGATED &&
// MOBILEGL_PIPE_PUSH`) was the same mistake the monolith arm's callers carried: the
// MONOLITH arm is not a split-arm fallback, it is the arm the pull build runs. An indexed or
// indirect draw over a client-memory array staged nothing there either, so the question has
// to be answerable - and answered - in the transport-free library too. The plan itself is
// pure arithmetic over the draw's own words; what changes per flavor is only who reads the
// bytes (the caller's reader), never the answer.
#pragma once

#include <MG_Pipe/MGPipeTypes.h>
#include <MG_State/GLState/BufferState/BufferObject.h>
#include <algorithm>
#include <cstring>
#include <limits>

namespace MobileGL::MG_Pipe {

    // The indirect block as a role that reads GL state can state it: the byte geometry plus
    // whether the GL_PARAMETER_BUFFER of the *IndirectCount forms is bound. MGPDrawIndirect's
    // two handle fields are the wire's identity and are meaningless to the monolith arm, which
    // resolves the very same bytes out of the frontend binding slots.
    struct MGPipeClientIndirect {
        Uint64 Offset = 0;
        Uint64 ParameterOffset = 0;
        Uint32 Stride = 0;
        Uint32 DrawCount = 0;
        Bool HasParameterBuffer = false;
    };

    // The draw's own shape, spelled the way the range record spells it: arrays carry Start =
    // first and no index size, indexed carries Start = the element offset into the element
    // buffer and IndexBias = base vertex.
    struct MGPipeClientDrawInputs {
        Uint8 IndexSize = 0;
        const MGPDrawRange* Ranges = nullptr;
        Uint32 RangeCount = 0;
        Uint32 InstanceCount = 1;
        Uint32 BaseInstance = 0;
        // True when primitive restart is live and RestartIndex is the value the index stream
        // restarts on. A restart index is a marker, not a vertex: it is what the mono arm's
        // restart substitution rewrites around, and counting it as a fetched element would
        // copy an element no primitive ever reads.
        Bool PrimitiveRestart = false;
        Uint32 RestartIndex = 0;
        // The application's own index array, when no element buffer is bound. Null with
        // IndexSize != 0 means "the indices come from the bound element buffer", i.e. through
        // the reader.
        const void* ClientIndices = nullptr;
        Uint64 ClientIndexBytes = 0;
        const MGPipeClientIndirect* Indirect = nullptr;
        // Whether a vertex-rate (divisor == 0) client array is present. A draw whose client
        // arrays are all instance-rate never has to read the index stream to answer "which
        // elements", so the element buffer is left alone (and the reader is never called with
        // BufferKind::Element).
        Bool WantVertices = false;
    };

    // Where a byte request is aimed. Both kinds are buffers a GL context names; they are
    // distinguished because the two roles resolve them from different places and a reader
    // that answered one for the other would size an upload off the wrong bytes.
    enum class MGPipeClientBufferKind : Uint8 {
        Element,        // the bound GL_ELEMENT_ARRAY_BUFFER, for the index stream
        IndirectCommand,// the bound GL_DRAW_INDIRECT_BUFFER, for the command block
        IndirectCount,  // the bound GL_PARAMETER_BUFFER, for an *IndirectCount form's count
    };

    // Reads `size` bytes at `offset` of the named buffer on the CPU. False when the bytes do
    // not exist on this side, which is a refusal rather than a guess.
    using MGPipeClientBufferReader = Bool (*)(void* user, MGPipeClientBufferKind kind, Uint64 offset, SizeT size,
                                              void* destination);

    // One sub-draw's fetch: the element range it reads, how many instances it runs, and the
    // raw base instance it shifts by.
    struct MGPipeClientFetch {
        MGPDrawRange Range;
        Uint32 Instances = 1;
        Uint32 BaseInstance = 0;
    };

    // The reader for a buffer whose bytes this side owns - the wire client's staged shadow and
    // the monolith arm's frontend object are both read this way. Reconciles a pending GPU write
    // first, because a shader-written index or command block is exactly the case this plan
    // exists to handle.
    inline Bool MGPipeReadBufferShadow(const SharedPtr<MG_State::GLState::BufferObject>& buffer, Uint64 offset,
                                       SizeT size, void* destination) {
        if (!buffer || offset > buffer->GetSize() || size > buffer->GetSize() - offset) return false;
        buffer->SyncGpuWrites();
        buffer->DownloadSubData(destination, static_cast<SizeT>(offset), size);
        return true;
    }

    class MGPipeClientFetchPlan {
    public:
        // False when the draw's fetched elements cannot be determined - an index or command
        // block this side cannot read, an index stream that leaves the client array, an index
        // that runs below the base vertex. A caller must then refuse the draw rather than
        // snapshot a guess: the elements it would leave out are the ones the draw reads.
        Bool Build(const MGPipeClientDrawInputs& inputs, MGPipeClientBufferReader reader, void* user) {
            m_inputs = inputs;
            m_fetches.clear();
            m_vertices.clear();
            if (inputs.Indirect != nullptr) {
                if (!ReadIndirectFetches(inputs, reader, user)) return false;
            } else {
                m_fetches.reserve(inputs.RangeCount);
                for (Uint32 i = 0; i < inputs.RangeCount; ++i) {
                    m_fetches.push_back({inputs.Ranges[i], inputs.InstanceCount, inputs.BaseInstance});
                }
            }
            if (!inputs.WantVertices) return true;
            return ReadVertices(inputs, reader, user);
        }

        // The vertex elements the draw fetches through a vertex-rate array, ascending and
        // unique. Only meaningful after a Build that wanted them.
        const Vector<Uint64>& Vertices() const { return m_vertices; }

        // The elements an attribute of `divisor` fetches: the vertex-rate set for divisor 0,
        // and one element per INSTANCE (baseInstance + instance / divisor) otherwise. Sorted
        // and unique, and `out` is left empty for a draw that fetches nothing.
        Bool Elements(Uint32 divisor, Vector<Uint64>& out) const {
            out.clear();
            if (divisor == 0) {
                // Vertex rate: every instance reads the same vertices, and the collection above
                // already deduplicated them across the draw's sub-ranges.
                out = m_vertices;
                return true;
            }
            for (const auto& fetch : m_fetches) {
                if (!fetch.Range.Count || !fetch.Instances) continue;
                const Uint64 first = fetch.BaseInstance;
                const Uint64 last = first + (fetch.Instances - 1u) / divisor;
                for (Uint64 at = first; at <= last; ++at) out.push_back(at);
            }
            std::sort(out.begin(), out.end());
            out.erase(std::unique(out.begin(), out.end()), out.end());
            return true;
        }

        const Vector<MGPipeClientFetch>& Fetches() const { return m_fetches; }

    private:
        Bool ReadIndirectFetches(const MGPipeClientDrawInputs& inputs, MGPipeClientBufferReader reader, void* user) {
            const MGPipeClientIndirect& indirect = *inputs.Indirect;
            Uint32 count = indirect.DrawCount;
            if (indirect.HasParameterBuffer) {
                Uint32 actual = 0;
                if (reader == nullptr ||
                    !reader(user, MGPipeClientBufferKind::IndirectCount, indirect.ParameterOffset, sizeof(actual),
                            &actual)) {
                    return false;
                }
                count = std::min(count, actual);
            }
            const Uint64 commandBytes = inputs.IndexSize ? 5 * sizeof(Uint32) : 4 * sizeof(Uint32);
            const Uint64 stride = indirect.Stride ? indirect.Stride : commandBytes;
            m_fetches.reserve(count);
            for (Uint32 i = 0; i < count; ++i) {
                const Uint64 displacement = static_cast<Uint64>(i) * stride;
                if (displacement > std::numeric_limits<Uint64>::max() - indirect.Offset) return false;
                Uint32 words[5]{};
                if (reader == nullptr ||
                    !reader(user, MGPipeClientBufferKind::IndirectCommand, indirect.Offset + displacement,
                            commandBytes, words)) {
                    return false;
                }
                Int32 bias = 0;
                if (inputs.IndexSize) std::memcpy(&bias, &words[3], sizeof(bias));
                m_fetches.push_back({{words[2], words[0], bias}, words[1], words[inputs.IndexSize ? 4 : 3]});
            }
            return true;
        }

        // The index stream's own answer. Arrays are contiguous by construction; an indexed
        // draw has to read the indices to learn which vertices it names, which is the whole
        // reason a GPU-written element buffer makes this a readback rather than arithmetic.
        Bool ReadVertices(const MGPipeClientDrawInputs& inputs, MGPipeClientBufferReader reader, void* user) {
            for (const auto& fetch : m_fetches) {
                if (!fetch.Range.Count || !fetch.Instances) continue;
                if (!inputs.IndexSize) {
                    for (Uint64 n = 0; n < fetch.Range.Count; ++n)
                        m_vertices.push_back(static_cast<Uint64>(fetch.Range.Start) + n);
                    continue;
                }
                const Uint64 offset = static_cast<Uint64>(fetch.Range.Start) * inputs.IndexSize;
                const Uint64 size = static_cast<Uint64>(fetch.Range.Count) * inputs.IndexSize;
                if (size > std::numeric_limits<SizeT>::max()) return false;
                Vector<Uint8> indexBytes;
                const Uint8* data = nullptr;
                if (inputs.ClientIndices != nullptr) {
                    if (offset > inputs.ClientIndexBytes || size > inputs.ClientIndexBytes - offset) return false;
                    data = static_cast<const Uint8*>(inputs.ClientIndices) + static_cast<SizeT>(offset);
                } else {
                    indexBytes.resize(static_cast<SizeT>(size));
                    if (reader == nullptr ||
                        !reader(user, MGPipeClientBufferKind::Element, offset, indexBytes.size(),
                                indexBytes.data())) {
                        return false;
                    }
                    data = indexBytes.data();
                }
                for (Uint32 n = 0; n < fetch.Range.Count; ++n) {
                    Uint32 index = 0;
                    std::memcpy(&index, data + static_cast<SizeT>(n) * inputs.IndexSize, inputs.IndexSize);
                    if (inputs.PrimitiveRestart && index == inputs.RestartIndex) continue;
                    const Int64 vertex = static_cast<Int64>(index) + fetch.Range.IndexBias;
                    if (vertex < 0) return false;
                    m_vertices.push_back(static_cast<Uint64>(vertex));
                }
            }
            std::sort(m_vertices.begin(), m_vertices.end());
            m_vertices.erase(std::unique(m_vertices.begin(), m_vertices.end()), m_vertices.end());
            return true;
        }

        MGPipeClientDrawInputs m_inputs{};
        Vector<MGPipeClientFetch> m_fetches;
        Vector<Uint64> m_vertices;
    };
} // namespace MobileGL::MG_Pipe
