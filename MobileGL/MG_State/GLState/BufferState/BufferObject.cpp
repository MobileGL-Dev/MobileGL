// MobileGL - MobileGL/MG_State/GLState/BufferState/BufferObject.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "BufferObject.h"

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

    void BufferObject::Respecify(SizeT size, const void* data) {
        ReleaseMemory();
        m_size = size;
        m_resource.ResizeShadow(size);
        if (data && size > 0) {
            Memcpy(m_resource.Bytes(), data, size);
        }
        // A NULL-data respecify (the orphaning idiom) leaves the store undefined;
        // record that so backends skip uploading the stale shadow bytes.
        m_hasDefinedContent = (data != nullptr) || size == 0;
        m_isImmutableStorage = false;
        m_storageFlags = 0;
        NotifyRespecify();
    }

    void BufferObject::Resize(SizeT size) {
        Respecify(size, nullptr);
    }

    void BufferObject::AllocateImmutableStorage(SizeT size, const void* data, GLbitfield storageFlags) {
        ReleaseMemory();
        m_size = size;
        m_resource.ResizeShadow(size);
        if (data) {
            Memcpy(m_resource.Bytes(), data, size);
        } else if (size > 0) {
            Memset(m_resource.Bytes(), 0, size);
        }
        m_hasDefinedContent = true;
        m_isImmutableStorage = true;
        m_storageFlags = storageFlags;
        NotifyRespecify();
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
                    Memcpy(m_resource.Bytes() + m_mappedRange.start, m_stagingData.data(),
                           m_mappedRange.end - m_mappedRange.start);
                }
                NotifyFlushMappedRange(m_mappedRange, m_mappingAccess);
            }

            m_stagingData.clear();
        }

        m_isMapped = false;
        m_mappingAccess = BufferMappingAccessBit::Null;
        m_mappedRange = {0, 0};
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
            Memcpy(m_resource.Bytes() + start, m_stagingData.data() + offset, length);
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

        Memcpy(m_resource.Bytes() + atOffset, data.data, data.size);
        NotifyContentWrite(atOffset, data.size);
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
            m_stagingData.resize(range.end - range.start);
            m_ownsStagingData = true;

            if (!(access & (BufferMappingAccessBit::InvalidateRange | BufferMappingAccessBit::InvalidateBuffer))) {
                Memcpy(m_stagingData.data(), m_resource.Bytes() + range.start, m_stagingData.size());
            }

            return m_stagingData.data();
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
            return const_cast<Uint8*>(m_stagingData.data());
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
