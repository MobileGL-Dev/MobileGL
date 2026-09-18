// MobileGL - MobileGL/MG_State/GLState/BufferState/PipeResource.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_Util/Types.h>
#include <bit>
#include <new>
#include <vector>

namespace MobileGL::MG_State::GLState {
    // GL_MIN_MAP_BUFFER_ALIGNMENT. GL 4.2 / ARB_map_buffer_alignment fix the minimum at 64 and
    // MobileGL advertises exactly that (MG_Impl/GLImpl/Getter/GL_Getter.cpp reads this constant),
    // so under-reporting is not available - the implementation has to be brought up to the number
    // instead. The promise is about POINTERS, not just the query: glMapBuffer must return a
    // 64-byte-aligned pointer, and glMapBufferRange must return one whose base - the returned
    // pointer minus the offset the caller asked for - is. Every pointer the frontend hands out
    // comes from the shadow below or from BufferObject's staging buffer, and std::vector only
    // promises alignof(std::max_align_t) (16 on aarch64), so both allocations carry the alignment
    // themselves. One constant for the getter and the allocator, because the two may never
    // disagree - the same reason the atomic-counter limits are shared through
    // MG_Util/ShaderTranspiler/Types.h.
    inline constexpr SizeT MIN_MAP_BUFFER_ALIGNMENT = 64;

    // The shadow's ALLOCATION alignment is a full page, not the advertised 64: a
    // page-aligned shadow (page-granular in SIZE as well, in a split build - see
    // ShadowAllocationBytesFor) owns every byte of every page it occupies, which is what
    // lets the persistent-map mprotect tracker write-protect the mapped range's containing
    // pages without ever covering a neighbour allocation's byte (a foreign thread with
    // signals blocked faulting on a shared page is a process kill).
    // GL_MIN_MAP_BUFFER_ALIGNMENT keeps answering 64 - over-aligning an allocation is
    // invisible to the application, and a glMapBufferRange base that is page-aligned is
    // 64-aligned. The cost is allocator rounding on small buffers, paid so the tracker
    // never has to make the unsafe choice.
    inline constexpr SizeT SHADOW_ALLOCATION_ALIGNMENT = 4096;

    // The bytes one shadow allocation actually asks the heap for, from the count std::vector
    // asked the allocator for. In a split build the SIZE is rounded up to a whole number of
    // SHADOW_ALLOCATION_ALIGNMENT pages, not just the base: alignment alone puts the first
    // byte on a page boundary but lets the LAST page end mid-way, and whatever the heap
    // packs after it shares that page. The persistent-map tracker then had to align its
    // protected range inward and hash the partial-page edges on every draw (8.5% of the
    // client thread at VD12 in XXH3, on nearly every buffer - almost no mapped end is
    // page-aligned). With a page-granular size every byte of [base, base + this) is this
    // allocation's own, so the tracker can protect the mapped range's containing pages
    // outward and there are no edges to hash. ONE function for the allocator and for
    // PipeResource::ShadowAllocationBytes(), because the extent the tracker widens into may
    // never exceed what was allocated. The pull build keeps the exact count and does not
    // see this function at all: it has no tracker to spend the rounding on, and G1 holds
    // its symbol set and .text byte-identical only if allocate()'s own text does not move.
#if MOBILEGL_BUILD_DISAGGREGATED
    inline constexpr SizeT ShadowAllocationBytesFor(SizeT count) {
        return (count + SHADOW_ALLOCATION_ALIGNMENT - 1) & ~(SHADOW_ALLOCATION_ALIGNMENT - 1);
    }
#endif

    // Allocator that gives every allocation SHADOW_ALLOCATION_ALIGNMENT (and, in a split
    // build, a page-granular size - see ShadowAllocationBytesFor). Deliberately minimal: the
    // vectors it backs hold raw bytes and are only ever sized, so allocate/deallocate plus the
    // rebinding and equality boilerplate std::vector requires is the whole interface.
    template <typename T>
    struct MapAlignedAllocator {
        using value_type = T;

        MapAlignedAllocator() noexcept = default;
        template <typename U>
        MapAlignedAllocator(const MapAlignedAllocator<U>&) noexcept {}

        T* allocate(SizeT count) {
            if (count == 0) return nullptr;
#if MOBILEGL_BUILD_DISAGGREGATED
            return static_cast<T*>(::operator new(ShadowAllocationBytesFor(count * sizeof(T)),
                                                  std::align_val_t{SHADOW_ALLOCATION_ALIGNMENT}));
#else
            return static_cast<T*>(
                ::operator new(count * sizeof(T), std::align_val_t{SHADOW_ALLOCATION_ALIGNMENT}));
#endif
        }
        void deallocate(T* pointer, SizeT) noexcept {
            ::operator delete(pointer, std::align_val_t{SHADOW_ALLOCATION_ALIGNMENT});
        }

        template <typename U>
        Bool operator==(const MapAlignedAllocator<U>&) const noexcept {
            return true;
        }
        template <typename U>
        Bool operator!=(const MapAlignedAllocator<U>&) const noexcept {
            return false;
        }
    };

    // Byte store for anything the application may end up holding a mapped pointer into.
    using MapAlignedData = std::vector<Uint8, MapAlignedAllocator<Uint8>>;

    // Opaque, refcounted handle to the backend's GPU storage for one buffer
    // (the driver-side resource). The active backend derives from it and attaches
    // its own payload (VkBufferResource / GLESBufferResource). Held by PipeResource.
    class BackendBufferResource {
    public:
        virtual ~BackendBufferResource() = default;
    };

    // Mesa pipe_resource analogue for a GL buffer's storage. It owns the buffer's
    // bytes and its backend GPU resource, and abstracts WHERE the authoritative
    // bytes live so no caller has to branch on the mode:
    //
    //  - Shadow mode (default, non-persistent buffers): the bytes live in a CPU
    //    Vector (the shadow). GL writes mutate the shadow; the active backend keeps
    //    its own GPU copy in sync via BufferBackendOps (glBufferData/SubData/...).
    //
    //  - Persistent mode (coherent GL_MAP_PERSISTENT maps): the bytes live in the
    //    backend's host-visible, COHERENT, persistently-mapped GPU memory. That GPU
    //    buffer is the single source of truth - the app writes into it directly,
    //    every read/write resolves against it, and NO per-write backend transfer
    //    happens. The CPU shadow is released on adoption.
    //
    // Bytes() always returns a host-visible base pointer valid for [0, size) in both
    // modes, so readers/writers just call Bytes() (the size lives on the owning
    // BufferObject). (Named Bytes(), not Data(), to avoid colliding with the type
    // alias Data = Vector<Uint8> used for the shadow.)
    class PipeResource {
    public:
        Uint8* Bytes() { return m_gpuMapped != nullptr ? static_cast<Uint8*>(m_gpuMapped) : m_shadow->data(); }
        const Uint8* Bytes() const {
            return m_gpuMapped != nullptr ? static_cast<const Uint8*>(m_gpuMapped) : m_shadow->data();
        }

        // True once the buffer's bytes have been adopted into backend GPU memory.
        Bool IsGpuResident() const { return m_gpuMapped != nullptr; }

        // Shadow (re)allocation for non-persistent storage (glBufferData /
        // glBufferStorage before any persistent map). Mirrors the previous
        // power-of-two reserve + exact resize of the old m_dataPtr.
        void ResizeShadow(SizeT size) {
            const SizeT reserved = std::bit_ceil(size == 0 ? SizeT{1} : size);
            m_shadow->reserve(reserved);
            m_shadow->resize(size);
#if MOBILEGL_BUILD_DISAGGREGATED
            // RECORDED HERE, NOT INFERRED FROM capacity(). `reserved` is the count this
            // call asked the allocator for, so ShadowAllocationBytesFor(reserved) is the
            // block the allocator handed back whenever this reserve reallocated - and when
            // it did not (the vector already held at least that much), the block behind
            // Bytes() is an earlier, LARGER reserve's, so the value recorded here is at
            // most the true extent either way. Reading capacity() back instead would tie
            // the ownership claim to a library's promise that capacity() never exceeds
            // the count it allocated - true of libc++, libstdc++ and MSVC today, pinned by
            // nothing - and an over-report there would protect a page this allocation
            // does not own, the very process-killer the extent exists to keep out. An
            // under-report costs nothing: the tracker still needs rangeEnd <= extent, and
            // size <= reserved always.
            m_shadowExtent = ShadowAllocationBytesFor(reserved);
#endif
        }
        // Direct shadow access, used only by the backend's upload-from-shadow path,
        // which never runs for a GPU-resident (persistent) buffer.
        MapAlignedData& Shadow() { return *m_shadow; }
        const MapAlignedData& Shadow() const { return *m_shadow; }

#if MOBILEGL_BUILD_DISAGGREGATED
        // The extent of the shadow's heap allocation in bytes, as ResizeShadow asked for
        // it: every byte of [Bytes(), Bytes() + this) belongs to this shadow and to
        // nothing else, and the extent is a whole number of SHADOW_ALLOCATION_ALIGNMENT
        // pages. This is what lets the persistent-map tracker protect the mapped range's
        // containing pages OUTWARD without ever covering a byte it does not own (a
        // foreign thread with signals blocked faulting on a shared page is a process
        // kill). Zero for an adopted (GPU-resident) store, whose shadow was released, and
        // for a shadow that was never sized; the tracker excludes both.
        SizeT ShadowAllocationBytes() const { return m_gpuMapped != nullptr ? 0 : m_shadowExtent; }
#endif

        // Transition to persistent GPU residency: adopt the backend's coherent
        // mapped base as the source of truth and drop the CPU shadow. The caller
        // must have already seeded the GPU memory from the shadow (via the backend
        // AcquirePersistentMap op) before calling this.
        void AdoptPersistentMap(void* mappedBase) {
            m_gpuMapped = mappedBase;
            m_shadow->clear();
            m_shadow->shrink_to_fit();
#if MOBILEGL_BUILD_DISAGGREGATED
            // The block is gone with the shrink; the next ResizeShadow records the next one.
            m_shadowExtent = 0;
#endif
        }

        // Give the adoption back: the bytes resolve against the shadow again (which
        // the caller must (re)size, it was released on adoption). Used when the store
        // itself is redefined - the mapping describes exactly the store that is going
        // away, so it may neither be written through nor kept. It is NOT a general
        // "unmap": a persistent map the application holds outlives every unmap by
        // definition, and the calls that could redefine such a buffer's store are
        // errors the frontend refuses before reaching here.
        void ReleasePersistentMap() { m_gpuMapped = nullptr; }

        // Backend GPU resource, owned here in both modes.
        const SharedPtr<BackendBufferResource>& Backend() const { return m_backend; }
        void SetBackend(SharedPtr<BackendBufferResource> backend) { m_backend = std::move(backend); }
        SharedPtr<BackendBufferResource> ReleaseBackend() { return std::move(m_backend); }

    private:
        // MapAlignedData, not Data: a read-only glMapBuffer hands the application this very
        // pointer, and a range map hands it base + offset, so the base has to be on the
        // GL_MIN_MAP_BUFFER_ALIGNMENT grid for either to satisfy ARB_map_buffer_alignment.
        SharedPtr<MapAlignedData> m_shadow = MakeShared<MapAlignedData>();
        void* m_gpuMapped = nullptr;
        SharedPtr<BackendBufferResource> m_backend;
#if MOBILEGL_BUILD_DISAGGREGATED
        // See ShadowAllocationBytes. Split-only so the pull build's layout does not move (G1).
        SizeT m_shadowExtent = 0;
#endif
    };
} // namespace MobileGL::MG_State::GLState
