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
            // Contents update of an ADOPTED (GPU-resident) store. `data` holds the app's
            // bytes, valid for the duration of the call only (a write map's staging
            // store is freed the moment the unmap that lands it returns); the frontend
            // has NOT touched the resident mapping. GL orders a
            // glBufferSubData after already-submitted GPU reads of the store, and an
            // in-place host write into the coherent mapping tears the frames still
            // reading the old bytes (Minecraft patches LIVE chunk sections this way -
            // the tear shows as one-frame wrong geometry/UVs during fast movement). The
            // backend lands the bytes on the GPU timeline instead: after in-flight
            // readers, before the next consumer. The frontend marks the buffer
            // gpu-write-pending so reads reconcile through ReadbackFromGpu. Backends
            // without this op keep the legacy ordered in-place host write.
            void (*ResidentSubData)(BufferObject& bufferObject, SizeT offset, DataPtr data) = nullptr;
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
            // Repeats one already-converted element through [atOffset, atOffset + size) and
            // publishes the range as one content mutation.
            void FillSubData(DataPtr pattern, SizeT atOffset, SizeT size);
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
            // resident, while the buffer is mapped (adoption releases the shadow a
            // mapping may have handed the application), or when the backend declines.
            Bool EnsureGpuResidentStorage();
            // Unmap. A write map's staged bytes land in the store on the way out, unless
            // the caller is about to replace that store (a respecification) and passes
            // false - landing them there would copy a whole mapped range into storage
            // being handed back on the next line.
            void ReleaseMemory(Bool landStagedWrites = true);
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
            // Globally-unique, never-reused id for THIS object's lifetime - same contract
            // and same motivation as ProgramObject::GetLifetimeId() and
            // VertexArrayObject::GetLifetimeId(). A backend that folds a buffer's IDENTITY
            // into a cache key must use this, never the GL name (LIFO-recycled by
            // glGenBuffers) and never the heap address (recycled by the allocator): both
            // let a deleted-and-recreated buffer answer to a dead one's cache entry.
            Uint64 GetLifetimeId() const { return m_lifetimeId; }
            // Monotonic counter bumped on every shadow mutation; backends use it to
            // validate cached transient slices.
            Uint64 GetChangeSerial() const;
            // False after a NULL-data (re)specification until the first content
            // write: the app's orphaning idiom (glBufferData with nullptr) leaves
            // the store undefined, so backends may (re)allocate GPU storage without
            // uploading the stale CPU shadow.
            Bool HasDefinedContent() const;

            const SharedPtr<BackendBufferResource>& GetBackendResource() const;
            void SetBackendResource(SharedPtr<BackendBufferResource> resource);

        private:
            // Sizes the store for a (re)definition, renewing an adopted GPU-resident
            // mapping across it. See the definition for why the renewal is not optional.
            void RedefineStorage(SizeT size);
            // Backend-initiated coherent adoption for mesh-arena-sized stores; see the
            // definition for the driver behavior that makes every other write route to
            // a busy large mutable store a frame-scale stall.
            void TryAdoptLargeStorage();
            void NotifyRespecify();
            void NotifySubData(SizeT offset, SizeT size);
            void NotifyFlushMappedRange(Range1D range, Flags<BufferMappingAccessBit> appAccess);
            // A content write of [offset, offset+size) just landed in m_resource. For a
            // persistent GPU-resident buffer the bytes are already in coherent GPU memory,
            // so this only bumps the change serial; otherwise it dispatches a backend
            // SubData transfer to sync the backend's separate GPU copy.
            void NotifyContentWrite(SizeT offset, SizeT size);
            // The one route CPU-sourced bytes take into an ADOPTED (GPU-resident) store:
            // glBufferSubData, a buffer clear, a buffer copy, and the landing of a
            // non-persistent write map at unmap / explicit flush all go through it, so
            // the routes cannot drift apart again. Carries no mapping asserts on
            // purpose - the unmap landing runs while the buffer is still mapped.
            void LandBytesIntoResidentStore(SizeT offset, DataPtr bytes);

            static Uint64 AllocateLifetimeId();

            const Uint m_externalIndex = 0;
            const Uint64 m_lifetimeId = AllocateLifetimeId();
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
            // See HasDefinedContent().
            Bool m_hasDefinedContent = true;
            // Set by MarkGpuWritten, cleared by SyncGpuWrites once the shadow is refreshed.
            Bool m_gpuWritePending = false;
            Range1D m_mappedRange;
            // The write-map staging store. MapAlignedData because the application is handed a
            // pointer into it, and biased by m_stagingBias because ARB_map_buffer_alignment
            // requires (returned pointer - offset) to be aligned, not the pointer itself: a range
            // map at offset 63 must hand back a pointer sitting 63 bytes past the alignment grid.
            // The bias is the offset's phase, so the mapped bytes still start at
            // m_stagingData.data() + m_stagingBias and the allocation is that much longer.
            MapAlignedData m_stagingData;
            SizeT m_stagingBias = 0;
            Bool m_ownsStagingData;
        };
    } // namespace MG_State::GLState
} // namespace MobileGL
