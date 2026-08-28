// MobileGL - MobileGL/MG_State/GLState/BufferState/BufferObject.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "BufferObject.h"

#include <Config.h>

#include <atomic>

namespace MobileGL::MG_State::GLState {
    namespace {
        const BufferBackendOps* g_bufferBackendOps = nullptr;
        // Starts at 1 so a zero-initialized cache slot can never carry a live buffer's id.
        std::atomic<Uint64> g_nextBufferLifetimeId{1};
    }

    Uint64 BufferObject::AllocateLifetimeId() {
        return g_nextBufferLifetimeId.fetch_add(1, std::memory_order_relaxed);
    }

    void SetBufferBackendOps(const BufferBackendOps* ops) {
        g_bufferBackendOps = ops;
    }

    const BufferBackendOps* GetBufferBackendOps() {
        return g_bufferBackendOps;
    }

    BufferObject::BufferObject(Uint externalIndex)
        : m_externalIndex(externalIndex), m_size(0), m_usage(BufferUsage::StaticDraw), m_isMapped(false),
          m_mappingAccess(BufferMappingAccessBit::Null), m_mappedRange({0, 0}), m_ownsStagingData{} {}

    BufferObject::~BufferObject() {
        if (m_resource.Backend() && g_bufferBackendOps && g_bufferBackendOps->OnDestroy) {
            g_bufferBackendOps->OnDestroy(m_resource.ReleaseBackend());
        }
    }

    void BufferObject::NotifyRespecify() {
        ++m_changeSerial;
        if (g_bufferBackendOps && g_bufferBackendOps->Respecify) {
            g_bufferBackendOps->Respecify(*this);
        }
    }

    void BufferObject::NotifySubData(SizeT offset, SizeT size) {
        ++m_changeSerial;
        if (size == 0) return;
        m_hasDefinedContent = true;
        if (g_bufferBackendOps && g_bufferBackendOps->SubData) {
            g_bufferBackendOps->SubData(*this, offset, size);
        }
    }

    void BufferObject::NotifyFlushMappedRange(Range1D range, Flags<BufferMappingAccessBit> appAccess) {
        ++m_changeSerial;
        if (range.start >= range.end) return;
        m_hasDefinedContent = true;
        if (g_bufferBackendOps && g_bufferBackendOps->FlushMappedRange) {
            g_bufferBackendOps->FlushMappedRange(*this, range, appAccess);
        }
    }

    void BufferObject::NotifyContentWrite(SizeT offset, SizeT size) {
        m_hasDefinedContent = true;
        if (m_resource.IsGpuResident()) {
            // The write already landed in coherent GPU memory; the backend has no separate
            // copy to sync. Only bump the serial so cached transient slices invalidate.
            ++m_changeSerial;
            return;
        }
        NotifySubData(offset, size);
    }

    // A (re)definition of the store is about to write `size` bytes through Bytes().
    // Sizing the shadow is all that takes for a shadow-backed buffer. A buffer whose
    // bytes were adopted into backend GPU memory has to give the adoption back first,
    // because the mapping it holds describes exactly the OLD store: writing the new
    // contents through it runs past its end the moment the store grows, and a backend
    // that replaces the storage for the new store - which is what an orphaning
    // respecification asks for - would leave that mapping, and therefore every later
    // read of this buffer, addressing storage nothing writes to any more. That was the
    // transform feedback capture that wrote one buffer while the readback read another.
    //
    // Given back rather than renewed here, deliberately. Renewing in place would mean
    // memcpying the new contents into storage that submitted-but-unretired draws may
    // still be reading, which is precisely what the orphaning idiom exists to avoid;
    // avoiding THAT would mean either stalling on a fence in the middle of a frame or
    // teaching the persistent-map op to orphan, and the op must never orphan for the
    // other kind of caller (an application-held GL_MAP_PERSISTENT_BIT mapping, whose
    // pointer has to stay valid for the buffer's whole life). Handing the store back to
    // the CPU shadow needs none of that: the backend's ordinary respecification path
    // then does the busy-tracking and the conditional orphan it has always done, and the
    // next binding that wants GPU residency takes a fresh mapping of the new store.
    void BufferObject::RedefineStorage(SizeT size) {
        if (m_resource.IsGpuResident()) {
            m_resource.ReleasePersistentMap();
            // Whatever a shader or a capture wrote is in the store being replaced, so
            // there is nothing left to reconcile - and leaving the flag set would make
            // the next read of this buffer wait for GPU work on behalf of bytes the
            // application has just thrown away.
            m_gpuWritePending = false;
        }
        m_size = size;
        m_resource.ResizeShadow(size);
    }

    void BufferObject::Respecify(SizeT size, const void* data) {
        ReleaseMemory();
        RedefineStorage(size);
        if (data && size > 0) {
            Memcpy(m_resource.Bytes(), data, size);
        }
        // A NULL-data respecify (the orphaning idiom) leaves the store undefined;
        // record that so backends skip uploading the stale shadow bytes.
        m_hasDefinedContent = (data != nullptr) || size == 0;
        m_isImmutableStorage = false;
        // GL 4.6 core 6.2 defines glBufferData as glBufferStorage with
        // DYNAMIC_STORAGE_BIT | MAP_READ_BIT | MAP_WRITE_BIT, so GL_BUFFER_STORAGE_FLAGS has to
        // report those three afterwards. Reporting 0 - the value that belongs to a buffer whose
        // store has never been specified - told an application that a perfectly writable
        // glBufferData buffer accepted neither glBufferSubData nor a map. Only the IMMUTABLE flag
        // distinguishes the two cases, and it is cleared just above.
        m_storageFlags = GL_DYNAMIC_STORAGE_BIT | GL_MAP_READ_BIT | GL_MAP_WRITE_BIT;
        NotifyRespecify();
        TryAdoptLargeStorage();
    }

    void BufferObject::Resize(SizeT size) {
        Respecify(size, nullptr);
    }

    void BufferObject::AllocateImmutableStorage(SizeT size, const void* data, GLbitfield storageFlags) {
        ReleaseMemory();
        RedefineStorage(size);
        if (data) {
            Memcpy(m_resource.Bytes(), data, size);
        } else if (size > 0) {
            Memset(m_resource.Bytes(), 0, size);
        }
        m_hasDefinedContent = true;
        m_isImmutableStorage = true;
        m_storageFlags = storageFlags;
        NotifyRespecify();
        TryAdoptLargeStorage();
    }

    // Back a LARGE store with the backend's persistently+coherently mapped GPU
    // storage the moment it is (re)defined, without waiting for the app to map it.
    // Minecraft 26.3 streams chunk meshes into 128MB vertex arenas with plain
    // glNamedBufferSubData - the one write API that carries no synchronization
    // hint - and on Mali every route that hands the driver a write into a busy
    // MUTABLE store either parks the calling thread (glBufferSubData, and
    // glMapBufferRange even with GL_MAP_UNSYNCHRONIZED_BIT) or ghost-copies the
    // whole destination on a driver worker (staged glCopyBufferSubData, and a
    // range-invalidating map: ~167ms per touched arena, the recurring in-world
    // hiccup). An adopted coherent map is the one shape with NO per-write driver
    // call at all: every SubData lands as a plain memcpy into GPU-visible memory,
    // and the shadow copy is dropped (a 128MB arena stops costing 128MB of RAM).
    // Only attempted for stores the size of mesh arenas: small buffers keep the
    // shadow model whose draw-time flush already prices them correctly.
    void BufferObject::TryAdoptLargeStorage() {
        constexpr SizeT kLargeBufferAdoptBytes = 16u * 1024u * 1024u;
        if (MG_Config::Features.DisableLargeBufferAdoption) return;
        if (m_size < kLargeBufferAdoptBytes) return;
        if (m_resource.IsGpuResident()) return;
        if (m_isMapped) return;
        if (g_bufferBackendOps == nullptr || g_bufferBackendOps->AcquirePersistentMap == nullptr) return;
        if (void* base = g_bufferBackendOps->AcquirePersistentMap(*this)) {
            m_resource.AdoptPersistentMap(base);
        }
    }

    void BufferObject::UploadData(DataPtr data, SizeT atOffset) {
        MOBILEGL_ASSERT(atOffset + data.size <= m_size,
                        "UploadData out of bounds: atOffset (%zu) + data.size (%zu) > m_size (%zu)", atOffset,
                        data.size, m_size);
        MOBILEGL_ASSERT(!m_isMapped || (m_mappingAccess & BufferMappingAccessBit::Persistent),
                        "Cannot upload data while buffer is non-persistently mapped.");
        Memcpy(m_resource.Bytes() + atOffset, data.data, data.size);
        NotifyContentWrite(atOffset, data.size);
    }

    void BufferObject::SetUsage(BufferUsage usage) {
        m_usage = usage;
    }

    void BufferObject::ReleaseMemory() {
        if (!m_isMapped) return;

        if (m_mappingAccess & BufferMappingAccessBit::Write) { // if we wrote to the buffer
            // A persistent GPU-resident map wrote straight into coherent GPU memory, so
            // there is nothing to copy back and no range to push down on unmap.
            if (!m_resource.IsGpuResident() &&
                !(m_mappingAccess & BufferMappingAccessBit::FlushExplicit)) { // if we didn't flush explicitly
                if (!(m_mappingAccess & BufferMappingAccessBit::Persistent)) {
                    Memcpy(m_resource.Bytes() + m_mappedRange.start, m_stagingData.data() + m_stagingBias,
                           m_mappedRange.end - m_mappedRange.start);
                }
                NotifyFlushMappedRange(m_mappedRange, m_mappingAccess);
            }

            m_stagingData.clear();
        }

        m_isMapped = false;
        m_mappingAccess = BufferMappingAccessBit::Null;
        m_mappedRange = {0, 0};
        m_stagingBias = 0;
        m_ownsStagingData = false;
    }

    void BufferObject::FlushMemoryRange(SizeT offset, SizeT length) {
        MOBILEGL_ASSERT(m_isMapped, "Buffer must be mapped to flush memory range.");
        MOBILEGL_ASSERT((m_mappingAccess & BufferMappingAccessBit::FlushExplicit),
                        "Buffer must be mapped with FlushExplicit access to flush memory range.");
        MOBILEGL_ASSERT((m_mappingAccess & BufferMappingAccessBit::Write),
                        "Buffer must be mapped with Write access to flush memory range.");

        SizeT start = m_mappedRange.start + offset;
        SizeT end = start + length;
        MOBILEGL_ASSERT(end <= m_mappedRange.end, "Flush range out of bounds: mappedRange.end (%zu) < end (%zu)",
                        m_mappedRange.end, end);

        // FLUSH_EXPLICIT maps are never GPU-resident (only coherent maps are adopted), so
        // the staged bytes must be copied into the shadow before the backend reads them.
        if (!(m_mappingAccess & BufferMappingAccessBit::Persistent)) {
            Memcpy(m_resource.Bytes() + start, m_stagingData.data() + m_stagingBias + offset, length);
        }
        NotifyFlushMappedRange({start, end}, m_mappingAccess);
    }

    void BufferObject::SyncPersistentMappedRange() {
        if (!m_isMapped) return;
        // GPU-resident: the app already wrote directly into coherent GPU memory. This is
        // the whole point of the persistent-map path - the per-draw whole-buffer re-upload
        // that used to run here is gone.
        if (m_resource.IsGpuResident()) return;
        if (!(m_mappingAccess & BufferMappingAccessBit::Persistent)) return;
        if (!(m_mappingAccess & BufferMappingAccessBit::Write)) return;
        if (m_mappingAccess & BufferMappingAccessBit::FlushExplicit) return;
        if (m_mappedRange.start >= m_mappedRange.end) return;

        NotifySubData(m_mappedRange.start, m_mappedRange.end - m_mappedRange.start);
    }

    void BufferObject::WritebackFromBackend(DataPtr data, SizeT atOffset) {
        MOBILEGL_ASSERT(atOffset + data.size <= m_size,
                        "WritebackFromBackend out of bounds: atOffset (%zu) + data.size (%zu) > m_size (%zu)", atOffset,
                        data.size, m_size);
        Memcpy(m_resource.Bytes() + atOffset, data.data, data.size);
        ++m_changeSerial;
    }

    void BufferObject::MarkGpuWritten() {
        m_hasDefinedContent = true;
        m_gpuWritePending = true;
    }

    void BufferObject::SyncGpuWrites() {
        if (!m_gpuWritePending) return;
        // Cleared unconditionally: without a readback op the shadow can never catch up,
        // and retrying on every subsequent read would only repeat the same no-op.
        m_gpuWritePending = false;
        if (m_size == 0 || g_bufferBackendOps == nullptr || g_bufferBackendOps->ReadbackFromGpu == nullptr) {
            return;
        }
        g_bufferBackendOps->ReadbackFromGpu(*this);
    }

    void BufferObject::UploadSubData(DataPtr data, SizeT atOffset) {
        // GL 4.6 core 6.5 forbids only the OVERLAPPING write: a glBufferSubData that stays
        // clear of a non-persistent mapping is legal, and the frontend lets it through.
        MOBILEGL_ASSERT(!m_isMapped || (m_mappingAccess & BufferMappingAccessBit::Persistent) ||
                            atOffset >= m_mappedRange.end || atOffset + data.size <= m_mappedRange.start,
                        "Cannot upload sub data overlapping a non-persistent mapping.");
        MOBILEGL_ASSERT(atOffset + data.size <= m_size,
                        "UploadSubData out of bounds: atOffset (%zu) + data.size (%zu) > m_size (%zu)", atOffset,
                        data.size, m_size);

        // An adopted store's Bytes() IS the memory the GPU reads, and a backend that
        // defers work (DirectVulkan's frame command buffer) may still be holding a
        // recorded-but-unsubmitted dispatch that GL orders this write AFTER. Writing
        // the mapping now would land the bytes underneath that dispatch - its
        // increments then execute on top of the newer data and invert the call order.
        // Retire the pending GPU writes first, as FillSubData already does. Shadow-
        // backed stores need none of this: the Memcpy below touches only the shadow,
        // and the backend's SubData op does its own ordering against in-flight work.
        if (m_resource.IsGpuResident()) {
            SyncGpuWrites();
        }

        Memcpy(m_resource.Bytes() + atOffset, data.data, data.size);
        NotifyContentWrite(atOffset, data.size);
    }

    void BufferObject::FillSubData(DataPtr pattern, SizeT atOffset, SizeT size) {
        MOBILEGL_ASSERT(pattern.data != nullptr && pattern.size > 0,
                        "FillSubData requires a non-empty pattern.");
        MOBILEGL_ASSERT(size % pattern.size == 0,
                        "FillSubData size (%zu) must be a multiple of pattern size (%zu).", size, pattern.size);
        MOBILEGL_ASSERT(atOffset <= m_size && size <= m_size - atOffset,
                        "FillSubData out of bounds: atOffset (%zu) + size (%zu) > m_size (%zu)", atOffset, size,
                        m_size);
        MOBILEGL_ASSERT(!m_isMapped || (m_mappingAccess & BufferMappingAccessBit::Persistent),
                        "Cannot fill data while buffer is non-persistently mapped.");
        if (size == 0) return;

        // A clear is ordered after all earlier GPU writes. Partial clears additionally need the
        // retained shadow bytes; whole-store clears need the same synchronization before writing
        // an adopted persistent mapping that the GPU may still be accessing.
        SyncGpuWrites();

        Uint8* dst = m_resource.Bytes() + atOffset;
        if (pattern.size == 1) {
            Memset(dst, *static_cast<const Uint8*>(pattern.data), size);
        } else {
            for (SizeT at = 0; at < size; at += pattern.size) {
                Memcpy(dst + at, pattern.data, pattern.size);
            }
        }
        NotifyContentWrite(atOffset, size);
    }

    void BufferObject::DownloadSubData(void* dst, SizeT atOffset, SizeT size) const {
        MOBILEGL_ASSERT(atOffset + size <= m_size,
                        "DownloadSubData out of bounds: atOffset (%zu) + size (%zu) > m_size (%zu)", atOffset, size,
                        m_size);
        Memcpy(dst, m_resource.Bytes() + atOffset, size);
    }

    void BufferObject::CopyDataFrom(const SharedPtr<BufferObject>& src, SizeT srcOffset, SizeT dstOffset, SizeT size) {
        MOBILEGL_ASSERT(!m_isMapped || (m_mappingAccess & BufferMappingAccessBit::Persistent),
                        "Cannot copy data while destination buffer is non-persistently mapped.");
        MOBILEGL_ASSERT(!src->IsMapped() || (src->GetMappingAccess() & BufferMappingAccessBit::Persistent),
                        "Cannot copy data from a buffer that is non-persistently mapped.");
        MOBILEGL_ASSERT(srcOffset + size <= src->GetSize(),
                        "Source buffer copy out of bounds: srcOffset (%zu) + size (%zu) > src->GetSize() (%zu)",
                        srcOffset, size, src->GetSize());
        MOBILEGL_ASSERT(dstOffset + size <= m_size,
                        "Destination buffer copy out of bounds: dstOffset (%zu) + size (%zu) > m_size (%zu)", dstOffset,
                        size, m_size);

        src->SyncGpuWrites();
        // The DESTINATION needs the same ordering as UploadSubData: an adopted store is
        // written in place, so pending recorded GPU writes to it must retire before the
        // copy lands or they would execute on top of it.
        if (m_resource.IsGpuResident()) {
            SyncGpuWrites();
        }
        Memcpy(m_resource.Bytes() + dstOffset, src->m_resource.Bytes() + srcOffset, size);
        NotifyContentWrite(dstOffset, size);
    }

    void* BufferObject::AcquireMemory(Bool markMapped, Bool read, Bool write) {
        SyncGpuWrites();
        if (markMapped) {
            m_isMapped = true;
            m_mappingAccess = (read ? BufferMappingAccessBit::Read : BufferMappingAccessBit::Null) |
                              (write ? BufferMappingAccessBit::Write : BufferMappingAccessBit::Null);
            m_mappedRange = {0, m_size};

            if (m_mappingAccess & BufferMappingAccessBit::Write) {
                // glMapBuffer maps from offset 0, so no bias: the allocation's own
                // GL_MIN_MAP_BUFFER_ALIGNMENT-aligned base is what the application must get.
                m_stagingBias = 0;
                m_stagingData.resize(m_size);
                m_ownsStagingData = true;

                if (!(m_mappingAccess &
                      (BufferMappingAccessBit::InvalidateRange | BufferMappingAccessBit::InvalidateBuffer))) {
                    Memcpy(m_stagingData.data(), m_resource.Bytes(), m_size);
                }

                return m_stagingData.data();
            }
        }

        return m_resource.Bytes();
    }

    Bool BufferObject::EnsureGpuResidentStorage() {
        if (m_resource.IsGpuResident()) {
            return true;
        }
        if (m_size == 0 || g_bufferBackendOps == nullptr || g_bufferBackendOps->AcquirePersistentMap == nullptr) {
            return false;
        }
        void* base = g_bufferBackendOps->AcquirePersistentMap(*this);
        if (base == nullptr) {
            return false;
        }
        m_resource.AdoptPersistentMap(base);
        return true;
    }

    void* BufferObject::AcquireMemoryRange(Range1D range, Flags<BufferMappingAccessBit> access) {
        MOBILEGL_ASSERT(range.end <= m_size && range.start <= range.end,
                        "AcquireMemoryRange out of bounds: range (%zu, %zu) exceeds m_size (%zu)", range.start,
                        range.end, m_size);
        // The app is about to look at the bytes; a shader may have rewritten them since
        // the shadow was last authoritative. Also needed for a write map without an
        // invalidate bit, whose staging copy is seeded from the shadow.
        SyncGpuWrites();
        m_isMapped = true;
        m_mappingAccess = access;
        m_mappedRange = range;

        if (access & BufferMappingAccessBit::Persistent) {
            m_ownsStagingData = false;
            // Zero-copy: for a coherent (non-FLUSH_EXPLICIT) persistent write map, ask the
            // active backend for host-visible, coherent GPU storage and adopt it as the
            // single source of truth. The backend seeds it from the current shadow before
            // returning; AdoptPersistentMap then releases the shadow. Falls back to the
            // shadow when the backend declines (returns null). Only attempted once - the
            // storage is immutable and outlives unmap/remap.
            if (!m_resource.IsGpuResident() && (access & BufferMappingAccessBit::Write) &&
                !(access & BufferMappingAccessBit::FlushExplicit) && g_bufferBackendOps &&
                g_bufferBackendOps->AcquirePersistentMap) {
                if (void* base = g_bufferBackendOps->AcquirePersistentMap(*this)) {
                    m_resource.AdoptPersistentMap(base);
                }
            }
            return m_resource.Bytes() + range.start;
        }

        if (access & BufferMappingAccessBit::Write) {
            // ARB_map_buffer_alignment constrains (returned pointer - offset), not the pointer:
            // a map at offset 63 must hand back a pointer 63 bytes past the alignment grid, which
            // is exactly what the read path below gets for free from shadowBase + offset. The
            // staging store has to be biased by the same phase to match, so it over-allocates by
            // it and the mapped bytes start at data() + m_stagingBias.
            m_stagingBias = range.start % MIN_MAP_BUFFER_ALIGNMENT;
            const SizeT mappedLength = range.end - range.start;
            m_stagingData.resize(m_stagingBias + mappedLength);
            m_ownsStagingData = true;

            if (!(access & (BufferMappingAccessBit::InvalidateRange | BufferMappingAccessBit::InvalidateBuffer))) {
                Memcpy(m_stagingData.data() + m_stagingBias, m_resource.Bytes() + range.start, mappedLength);
            }

            return m_stagingData.data() + m_stagingBias;
        } else {
            m_ownsStagingData = false;
            return m_resource.Bytes() + range.start;
        }
    }

    const Uint8* BufferObject::MappedData() const {
        return m_resource.Bytes();
    }

    Bool BufferObject::IsBackendPersistentMapped() const {
        return m_resource.IsGpuResident();
    }

    SizeT BufferObject::GetSize() const {
        return m_size;
    }

    Bool BufferObject::IsImmutableStorage() const {
        return m_isImmutableStorage;
    }

    BufferUsage BufferObject::GetUsage() const {
        return m_usage;
    }

    Uint64 BufferObject::GetChangeSerial() const {
        return m_changeSerial;
    }

    Bool BufferObject::HasDefinedContent() const {
        return m_hasDefinedContent;
    }

    const SharedPtr<BackendBufferResource>& BufferObject::GetBackendResource() const {
        return m_resource.Backend();
    }

    void BufferObject::SetBackendResource(SharedPtr<BackendBufferResource> resource) {
        m_resource.SetBackend(std::move(resource));
    }

    Bool BufferObject::IsMapped() const {
        return m_isMapped;
    }

    Range1D BufferObject::GetMappedRange() const {
        return m_isMapped ? m_mappedRange : Range1D{0, 0};
    }

    void* BufferObject::GetMappedPointer() const {
        if (!m_isMapped) return nullptr;
        if (m_mappingAccess & BufferMappingAccessBit::Persistent) {
            // GPU-resident maps return the coherent GPU pointer; shadow-backed persistent
            // maps return the shadow. m_resource.Bytes() resolves both.
            return const_cast<Uint8*>(m_resource.Bytes()) + m_mappedRange.start;
        }
        if (m_ownsStagingData) {
            return const_cast<Uint8*>(m_stagingData.data()) + m_stagingBias;
        }
        return const_cast<Uint8*>(m_resource.Bytes()) + m_mappedRange.start;
    }

    Flags<BufferMappingAccessBit> BufferObject::GetMappingAccess() const {
        return m_isMapped ? m_mappingAccess : BufferMappingAccessBit::Null;
    }

    GLbitfield BufferObject::GetStorageFlags() const {
        return m_storageFlags;
    }

    Uint BufferObject::GetExternalIndex() const {
        return m_externalIndex;
    }
} // namespace MobileGL::MG_State::GLState
