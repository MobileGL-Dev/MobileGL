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

            // ---- P5 b1: the two things a split build has to do that a monolith does not ---
            //
            // Everything here is behind the build option AND behind
            // `MG_Config::Transport != Monolith` at the call site, because an extra
            // resource_subdata record or an extra respecify on the monolith path is new
            // behaviour and D-J forbids it. The pull build compiles none of it, which is also
            // how G1 holds over a file this central.

            // ONE MOBILEGL_IPC_PERSISTENT_BLOCK_KB BLOCK of the mapped span, emitted through
            // the private NotifySubData - the same serial, the same defined-content flag, the
            // same record - so the split arm and the monolith arm differ in HOW the span is
            // cut and in nothing else. Called only by
            // MG_Record::PersistentMapTracker, which owns the cutting.
            void PushMappedSpanBlock(SizeT offset, SizeT size);

            // Called on every event that can move the tracker's membership predicate: map,
            // unmap, respecify, adoption, destruction. It is one call rather than an
            // insert/erase pair on purpose - the predicate is read from this object, so a
            // caller that had to decide which of the two to call could decide differently
            // from IsLivePersistentMap and the set would drift from the thing it models.
            //
            // It also PUBLISHES the live-host-writes bit when it changes: the server must
            // know that a write map is live, because such a map mutates the shadow with no
            // call, no serial and no epoch, and the applier's IsBufferDrawCleanByHandle can
            // no longer ask this object (there is no object on that side of a spawn).
            void NotePersistentMapStateChanged();

            // What MGPipeEmitResourceSubData and MGPipeEmitBufferSubDataResident write into
            // MGPSubData::HasLiveHostWrites. The PUBLISHED value, not the live predicate: the
            // two are the same by the time any content record is built, and reading the
            // published one is what makes a record and the edge that announced it agree by
            // construction.
            Bool HasLiveHostWritesForWire() const;

            // Is a GPU write still unreconciled? Under split this is the THIRD STATE made
            // readable: SyncGpuWrites no longer clears optimistically, so between the readback
            // emission and OnBufferWriteback landing this stays true, and nothing else in the
            // object can express that. Split-only so the pull build's layout and inlining do
            // not move (G1).
            Bool HasOutstandingGpuWrite() const { return m_gpuWritePending; }

            // The shadow allocation's own extent in bytes - page-aligned base, page-granular
            // size, every byte this shadow's and nobody else's (PipeResource.h,
            // ShadowAllocationBytesFor). Read by the persistent-map tracker at registration
            // so it can protect the mapped range's containing pages OUTWARD and hash no
            // edges; zero for an adopted store, which the tracker never registers. Split-only
            // for G1's reason, like the rest of this block, and out of line because it is a
            // layer-1 read of client memory like MappedData (CONTRACT-P5C rule E) and carries
            // the same apply-thread refusal.
            SizeT ShadowAllocationBytes() const;

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
            // P15: the pipe client's last Buffer handle for this object (MGPipeSlotAllocator::
            // AcquireHinted). Only a hint: it is checked against the allocator on every use.
            Uint64& PipeHandleHint() const { return m_pipeHandleHint; }
            // Monotonic counter bumped on every shadow mutation; backends use it to
            // validate cached transient slices.
            Uint64 GetChangeSerial() const;
            // False after a NULL-data (re)specification until the first content
            // write: the app's orphaning idiom (glBufferData with nullptr) leaves
            // the store undefined, so backends may (re)allocate GPU storage without
            // uploading the stale CPU shadow.
            Bool HasDefinedContent() const;

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
            mutable Uint64 m_pipeHandleHint = 0;
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
            // Set by MarkGpuWritten, cleared by SyncGpuWrites once the shadow is refreshed -
            // and, in a split build, cleared by WritebackFromBackend instead, because there
            // the answer arrives later than the request. See SyncGpuWrites' definition.
            Bool m_gpuWritePending = false;
            // The last value of MGPResourceDesc::HasLiveHostWrites this object published.
            // Behind the option so the pull build's layout - and therefore every inlined
            // constructor and accessor in it - does not move (G1).
            Bool m_publishedLiveHostWrites = false;
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
