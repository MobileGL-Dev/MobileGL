// MobileGL - MobileGL/MG_State/GLState/BufferState/BufferObject.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_Util/Math/VectorTypes.h>
#include "PipeResource.h"

namespace MobileGL {
    enum class BufferTarget {
        Vertex,
        Index,
        Uniform,
        CopyRead,
        CopyWrite,
        PixelPack,
        PixelUnpack,
        Query,
        Texture,
        TransformFeedback,
        AtomicCounter,
        DispatchIndirect,
        DrawIndirect,
        Parameter,
        ShaderStorage,
        BufferTargetCount,
        Unknown = -1
    };

    enum class BufferUsage {
        StreamDraw,
        StreamRead,
        StreamCopy,
        StaticDraw,
        StaticRead,
        StaticCopy,
        DynamicDraw,
        DynamicRead,
        DynamicCopy,
        Unknown = -1
    };

    enum class BufferMappingAccessBit : Uint {
        Null = 0x00,
        Read = 0x01,
        Write = 0x02,
        InvalidateRange = 0x04,
        InvalidateBuffer = 0x08,
        FlushExplicit = 0x10,
        Unsynchronized = 0x20,
        Persistent = 0x40,
        Coherent = 0x80
    };

    namespace MG_State::GLState {
        class BufferObject;

        // BackendBufferResource and PipeResource (the storage abstraction that holds
        // either the CPU shadow or the backend's persistently-mapped GPU memory) live
        // in PipeResource.h.

        // Immediate buffer transfer interface implemented by the active backend
        // (the pipe_context buffer-op analogue). Ops are invoked at GL call time,
        // right after the shadow copy has been updated; contents are always read
        // from the shadow so ops carry only ranges and flags.
        //
        // Every op must tolerate bufferObject.GetBackendResource() == nullptr:
        // resources are created lazily by the backend's draw/bind-time ensure
        // path, which performs a full upload from the shadow and thereby covers
        // all ops that happened before the resource existed.
        struct BufferBackendOps {
            // Storage (re)definition: glBufferData / glBufferStorage. The orphaning
            // point - the backend decides (busy-tracking) whether to swap storage
            // or write in place. Shadow already holds the new contents.
            void (*Respecify)(BufferObject& bufferObject) = nullptr;
            // Contents update of [offset, offset + size) from the shadow.
            void (*SubData)(BufferObject& bufferObject, SizeT offset, SizeT size) = nullptr;
            // Write-map flush (glUnmapBuffer / glFlushMappedBufferRange). Carries the
            // app's real mapping flags so the backend can honour INVALIDATE_* /
            // UNSYNCHRONIZED semantics per call instead of merging them.
            void (*FlushMappedRange)(BufferObject& bufferObject, Range1D range,
                                     Flags<BufferMappingAccessBit> appAccess) = nullptr;
            // Final release of the backend resource (called from ~BufferObject).
            // The backend defers actual destruction until the GPU is done with it.
            void (*OnDestroy)(SharedPtr<BackendBufferResource>&& resource) = nullptr;
            // Zero-copy persistent mapping. For a coherent (non-FLUSH_EXPLICIT) persistent
            // write map, the backend may hand back a host-visible, COHERENT, persistently
            // mapped pointer into its own GPU storage for the whole buffer [0, size),
            // created with every buffer usage and seeded from the shadow. From that point
            // the GPU buffer is the single source of truth: the app writes into it
            // directly, all reads/writes resolve against it (HostData()), and NO further
            // backend transfer ops are dispatched for this buffer. Returns nullptr when the
            // backend cannot back the map; the frontend then keeps the CPU-shadow model.
            // Must be idempotent: a second call for an already-backed buffer returns the
            // same base pointer.
            void* (*AcquirePersistentMap)(BufferObject& bufferObject) = nullptr;
            // Pulls the backend's current contents for the whole buffer into the shadow
            // (through WritebackFromBackend). Only ever called for a buffer the GPU may
            // have written behind the frontend's back - a shader storage or atomic counter
            // binding of a draw or dispatch - because nothing else can desynchronise the
            // shadow. Backends that cannot read their storage back leave this null; the
            // shadow then keeps its pre-dispatch bytes, which is the old behaviour.
            void (*ReadbackFromGpu)(BufferObject& bufferObject) = nullptr;
        };

        // Registered by the active backend at init, cleared at shutdown.
        // Null table (unit tests, benchmarks) => shadow-only state tracking.
        void SetBufferBackendOps(const BufferBackendOps* ops);
        const BufferBackendOps* GetBufferBackendOps();

        class BufferObject {
        public:
            using TargetEnum = BufferTarget;

            BufferObject(Uint externalIndex);
            ~BufferObject();

            BufferObject(const BufferObject&) = delete;
            BufferObject& operator=(const BufferObject&) = delete;

            // Storage definition (single backend Respecify): glBufferData.
            void Respecify(SizeT size, const void* data);
            // Storage definition without contents; equivalent to Respecify(size, nullptr).
            void Resize(SizeT size);
            void AllocateImmutableStorage(SizeT size, const void* data, GLbitfield storageFlags);
            void SetUsage(BufferUsage usage);

            void UploadData(DataPtr data, SizeT atOffset);
            void UploadSubData(DataPtr data, SizeT atOffset);
            // Reads `size` bytes from the CPU shadow at `atOffset` into `dst` (glGetBufferSubData).
            // The shadow reflects CPU writes (BufferData/SubData/maps) and backend write-backs, but not
            // arbitrary GPU-side writes.
            void DownloadSubData(void* dst, SizeT atOffset, SizeT size) const;
            void CopyDataFrom(const SharedPtr<BufferObject>& src, SizeT srcOffset, SizeT dstOffset, SizeT size);

            void* AcquireMemory(Bool markMapped, Bool read, Bool write);
            void* AcquireMemoryRange(Range1D range, Flags<BufferMappingAccessBit> access);
            // Adopt backend host-visible coherent GPU storage as the source of truth
            // (used for GPU-written targets like transform feedback capture, so
            // MapBuffer/GetBufferSubData read real GPU results). No-op when already
            // resident or when the backend declines.
            Bool EnsureGpuResidentStorage();
            void ReleaseMemory();
            void FlushMemoryRange(SizeT offset, SizeT length);

            // Pushes the persistently-mapped write range to the backend; called by
            // backends at draw time (persistent maps mutate the shadow without API calls).
            void SyncPersistentMappedRange();
            // Shadow-only write used when the backend copies GPU results (e.g. ReadPixels
            // into a pixel-pack buffer) back into the frontend mirror. Does not issue a
            // backend op: the backend storage already holds these bytes.
            void WritebackFromBackend(DataPtr data, SizeT atOffset);

            // A draw or dispatch just ran with this buffer bound where a shader can write
            // it (shader storage / atomic counter). The next read has to reconcile with
            // that: pull the bytes back, or - when the shadow already IS coherent GPU
            // memory - wait for the work that wrote them to retire. Which of the two is
            // the backend's business; the flag only says a GPU write is outstanding.
            void MarkGpuWritten();
            // Refreshes the shadow from the backend when a GPU write is outstanding. Called
            // from every path that reads the shadow on the app's behalf.
            void SyncGpuWrites();

            Bool IsMapped() const;
            Bool IsImmutableStorage() const;
            SizeT GetSize() const;
            BufferUsage GetUsage() const;
            Range1D GetMappedRange() const;
            void* GetMappedPointer() const;
            // Host-visible base pointer to the buffer's authoritative bytes for
            // [0, GetSize()): the coherent persistent GPU map when the buffer is
            // persistent-resident, otherwise the CPU shadow. Every reader goes through
            // this so no consumer branches on where the bytes live (the class of bug
            // that a partial persistent-map redirect would reintroduce).
            const Uint8* MappedData() const;
            // True once the buffer's bytes were adopted into backend GPU memory (a
            // coherent persistent map): reads/writes hit GPU memory and no per-write
            // backend transfer op is dispatched.
            Bool IsBackendPersistentMapped() const;
            Flags<BufferMappingAccessBit> GetMappingAccess() const;
            GLbitfield GetStorageFlags() const;
            Uint GetExternalIndex() const;
            // Monotonic counter bumped on every shadow mutation; backends use it to
            // validate cached transient slices.
            Uint64 GetChangeSerial() const;

            const SharedPtr<BackendBufferResource>& GetBackendResource() const;
            void SetBackendResource(SharedPtr<BackendBufferResource> resource);

        private:
            void NotifyRespecify();
            void NotifySubData(SizeT offset, SizeT size);
            void NotifyFlushMappedRange(Range1D range, Flags<BufferMappingAccessBit> appAccess);
            // A content write of [offset, offset+size) just landed in m_resource. For a
            // persistent GPU-resident buffer the bytes are already in coherent GPU memory,
            // so this only bumps the change serial; otherwise it dispatches a backend
            // SubData transfer to sync the backend's separate GPU copy.
            void NotifyContentWrite(SizeT offset, SizeT size);

            const Uint m_externalIndex = 0;
            SizeT m_size = 0;
            BufferUsage m_usage = BufferUsage::StaticDraw;
            // Owns the buffer's bytes (CPU shadow or backend persistent GPU map) and
            // the backend GPU resource. All data access goes through it.
            PipeResource m_resource;
            Bool m_isMapped;
            Flags<BufferMappingAccessBit> m_mappingAccess;
            Bool m_isImmutableStorage = false;
            GLbitfield m_storageFlags = 0;
            Uint64 m_changeSerial = 0;
            // Set by MarkGpuWritten, cleared by SyncGpuWrites once the shadow is refreshed.
            Bool m_gpuWritePending = false;
            Range1D m_mappedRange;
            Vector<Uint8> m_stagingData;
            Bool m_ownsStagingData;
        };
    } // namespace MG_State::GLState
} // namespace MobileGL
