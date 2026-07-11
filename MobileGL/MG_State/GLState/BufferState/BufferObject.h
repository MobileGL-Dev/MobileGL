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

        // Opaque, refcounted handle to the backend's storage for one buffer
        // (the pipe_resource analogue). The frontend owns the reference; the
        // active backend derives from it and attaches its own payload.
        class BackendBufferResource {
        public:
            virtual ~BackendBufferResource() = default;
        };

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
            void ReleaseMemory();
            void FlushMemoryRange(SizeT offset, SizeT length);

            // Pushes the persistently-mapped write range to the backend; called by
            // backends at draw time (persistent maps mutate the shadow without API calls).
            void SyncPersistentMappedRange();
            // App-compat for FLUSH_EXPLICIT persistent maps: engines like Flywheel
            // write copy descriptors into the mapping and bind that range as an SSBO
            // without ever flushing it. Undefined per spec, but works on drivers whose
            // persistent maps alias GPU-visible memory. Ours alias the CPU shadow, so
            // callers push the GPU-read range down right before it is consumed.
            void SyncMappedRangeForGpuRead(Range1D range);
            // Shadow-only write used when the backend copies GPU results (e.g. ReadPixels
            // into a pixel-pack buffer) back into the frontend mirror. Does not issue a
            // backend op: the backend storage already holds these bytes.
            void WritebackFromBackend(DataPtr data, SizeT atOffset);

            Bool IsMapped() const;
            Bool IsImmutableStorage() const;
            SizeT GetSize() const;
            BufferUsage GetUsage() const;
            Range1D GetMappedRange() const;
            void* GetMappedPointer() const;
            const SharedPtr<Data>& GetDataReadOnly() const;
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

            const Uint m_externalIndex = 0;
            SizeT m_size = 0;
            BufferUsage m_usage = BufferUsage::StaticDraw;
            SharedPtr<Data> m_dataPtr;
            Bool m_isMapped;
            Flags<BufferMappingAccessBit> m_mappingAccess;
            Bool m_isImmutableStorage = false;
            GLbitfield m_storageFlags = 0;
            Uint64 m_changeSerial = 0;
            Range1D m_mappedRange;
            Vector<Uint8> m_stagingData;
            Bool m_ownsStagingData;
            SharedPtr<BackendBufferResource> m_backendResource;
        };
    } // namespace MG_State::GLState
} // namespace MobileGL
