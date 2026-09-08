// MobileGL - MobileGL/MG_Impl/Pipe/ResourceTracker.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

// The CLIENT side of P3a's resource family (brief D-A, D-B, D-C, D-D).
//
// WHERE IT RUNS, and it is the ONE exception to push-at-validate (ARCHITECTURE.md 5.1):
// the seven BufferBackendOps hooks already dispatch at the GL call that causes them, so
// their pipe calls are emitted from the same BufferObject dispatchers - not from
// MGPipeValidateForVerb. Nothing about buffers moves to validate time in P3a.
//
// WHAT LIVES HERE
//   * the sticky BindMask, one constexpr BufferTarget -> bit table with a static_assert
//     that it covers every enumerator, so a new target cannot be silently unmapped;
//   * the lifetimeId -> {slot, gen} mint (through MGPipeSlots(), the one allocator) and
//     the slot -> BufferObject* INVERSE the reverse channel resolves a writeback through;
//   * the nine MGPipeEmitResource* bodies, declared in MG_Pipe/PipeMutation.h so that
//     MG_State sees a declaration and never this file (the same layering PipeMutation.h
//     already has for MGP_NOTE_MUTATION: declare in MG_Pipe, define in MG_Impl);
//   * the MGPSubData range splitter, because one record's box caps the destination at a
//     2^31-1 offset and a 2^32-1 size;
//   * the map-persistent-roundtrips counting site.
//
// HEADER-ONLY, for the ownership reason Tracker.h states in full: the root CMakeLists.txt
// that would name a new .cpp belongs to the contract package and is frozen behind the tag.
// MG_Impl/Pipe/PipeFill.cpp is the one translation unit that includes it in the library.
//
// NO TIMER, and no per-call record copy on a HOT path. The two observables a unit case
// needs - the last emitted descriptor and the per-call counts - are written only by
// resource_create and resource_respecify, which run once per glBufferData rather than per
// upload; resource_subdata, the hot one, is observed through the pure builders below
// instead (MGPipeBuildSubDataRecord / MGPipeForEachSubDataRecordRange), which is also what
// lets a test drive the splitter at both of its bounds without a 4 GiB buffer.
#if MOBILEGL_PIPE_PUSH
#include <MG_Impl/Pipe/SlotAllocator.h>
#include <MG_Pipe/MGPipe.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_Pipe/PipeMutation.h>
#include <MG_State/GLState/BufferState/BufferState.h>
#include <MG_State/GLState/Core.h>
#include <MG_Util/Metrics/PipeStats.h>

#include <Config.h>

#include <cstdint>

namespace MobileGL::MG_Pipe {

    // ---------------------------------------------------------------------------------
    // D-A3: BindMask
    // ---------------------------------------------------------------------------------

    // MGPResourceDesc::BindMask's twelve bits, in the order MGPipeTypes.h names them:
    // VERTEX|INDEX|CONSTANT|SHADER_BUFFER|INDIRECT|SAMPLER|SHADER_IMAGE|RENDER_TARGET|
    // DEPTH_STENCIL|STREAM_OUTPUT|ATOMIC|ELEMENT_ARRAY.
    //
    // They are spelled HERE rather than in MGPipeTypes.h because that header is the contract
    // package's and the mask has, so far, exactly one producer: this file. The integrator
    // moves them beside the field when a second producer appears (P4a's texture family).
    enum MGPipeBindBit : Uint16 {
        kMGPipeBindNone = 0,
        kMGPipeBindVertex = 1u << 0,
        kMGPipeBindIndex = 1u << 1,
        kMGPipeBindConstant = 1u << 2,
        kMGPipeBindShaderBuffer = 1u << 3,
        kMGPipeBindIndirect = 1u << 4,
        kMGPipeBindSampler = 1u << 5,
        kMGPipeBindShaderImage = 1u << 6,
        kMGPipeBindRenderTarget = 1u << 7,
        kMGPipeBindDepthStencil = 1u << 8,
        kMGPipeBindStreamOutput = 1u << 9,
        kMGPipeBindAtomic = 1u << 10,
        // THE D-B7 SWITCH. With kCapNeedsHostIndexBytes set the server mirrors this
        // resource's bytes so it can rewrite restart indices and flatten multi-draws
        // (ARCHITECTURE.md 10.3). Getting it wrong is invisible in monolith and silently
        // disables both under split, which is why it is set from the same table as every
        // other bit rather than from a special case at the emission site.
        kMGPipeBindElementArray = 1u << 11,
    };

    // A sentinel the table below returns for an enumerator it does not name. It is NOT a
    // legal mask value: every enumerator must be listed, including the ones that map to no
    // bit at all, so that ADDING a BufferTarget is a build break here rather than a bit
    // that silently stops being published.
    inline constexpr Uint32 kMGPipeBindUnmapped = 0x10000u;

    // The one table. No `default:` arm on purpose - that is what makes the static_assert
    // below able to see an unnamed enumerator.
    constexpr Uint32 MGPipeBindMaskForBufferTarget(BufferTarget target) {
        switch (target) {
        case BufferTarget::Vertex:
            return kMGPipeBindVertex;
        // GL_ELEMENT_ARRAY_BUFFER is the VAO's element slot: the same bind is both "this
        // resource is an index buffer" and "the server may need its bytes on its own side".
        case BufferTarget::Index:
            return kMGPipeBindIndex | kMGPipeBindElementArray;
        case BufferTarget::Uniform:
            return kMGPipeBindConstant;
        case BufferTarget::ShaderStorage:
            return kMGPipeBindShaderBuffer;
        case BufferTarget::DispatchIndirect:
        case BufferTarget::DrawIndirect:
        case BufferTarget::Parameter:
            return kMGPipeBindIndirect;
        // A texture buffer's backing store is SAMPLED through the texture that names it.
        case BufferTarget::Texture:
            return kMGPipeBindSampler;
        case BufferTarget::TransformFeedback:
            return kMGPipeBindStreamOutput;
        case BufferTarget::AtomicCounter:
            return kMGPipeBindAtomic;
        // TRANSFER AND QUERY TARGETS, which the bind mask deliberately does not name: none
        // of them is a pipeline binding, none of them makes the server keep anything, and
        // a bit set for them would only widen what a split server mirrors. Listed rather
        // than defaulted, so the completeness assert still sees them.
        case BufferTarget::CopyRead:
        case BufferTarget::CopyWrite:
        case BufferTarget::PixelPack:
        case BufferTarget::PixelUnpack:
        case BufferTarget::Query:
            return kMGPipeBindNone;
        case BufferTarget::BufferTargetCount:
        case BufferTarget::Unknown:
            return kMGPipeBindNone;
        }
        return kMGPipeBindUnmapped;
    }

    constexpr Bool MGPipeEveryBufferTargetIsMapped() {
        for (SizeT i = 0; i < static_cast<SizeT>(BufferTarget::BufferTargetCount); ++i) {
            if (MGPipeBindMaskForBufferTarget(static_cast<BufferTarget>(i)) == kMGPipeBindUnmapped) {
                return false;
            }
        }
        return true;
    }
    static_assert(MGPipeEveryBufferTargetIsMapped(),
                  "a BufferTarget enumerator has no MGPResourceDesc::BindMask row: add it to "
                  "MGPipeBindMaskForBufferTarget, including a deliberate kMGPipeBindNone, or the "
                  "resource it is bound to stops publishing that binding (D-A3, P8 expectation 1)");
    static_assert(MGPipeBindMaskForBufferTarget(BufferTarget::Index) & kMGPipeBindElementArray,
                  "the ELEMENT_ARRAY bit is the index host mirror's switch (ARCHITECTURE.md 10.3)");

    // ---------------------------------------------------------------------------------
    // The discriminators MGPResourceDesc / MGPSubData carry for a BUFFER
    // ---------------------------------------------------------------------------------
    //
    // MGPipeTypes.h documents Target as "Buffer | Tex1D..TexCubeArray | Renderbuffer |
    // TexBuffer" and StorageKind as "== TextureStorageType", but P3a is buffer-only and the
    // contract package minted no enum for the first list. Buffer is its leading member and
    // is therefore 0, which is also what a zero-initialised record already says; the second
    // is the frontend enum, named rather than open-coded.
    inline constexpr Uint16 kMGPipeResourceTargetBuffer = 0;
    inline constexpr Uint8 kMGPipeResourceStorageKindBuffer =
        static_cast<Uint8>(MobileGL::TextureStorageType::Buffer);

    // ---------------------------------------------------------------------------------
    // D-A2: the payload builders. Pure, so a unit case can assert field by field.
    // ---------------------------------------------------------------------------------

    // The descriptor for `buffer`. `storageDefined` is false for the create that the
    // constructor emits - storage is defined lazily by the first respecify and a backend
    // tolerates a resource that has none - and true for every respecify.
    inline MGPResourceDesc MGPipeBuildResourceDesc(const MG_State::GLState::BufferObject& buffer,
                                                   MGPipeHandle handle, Uint16 bindMask,
                                                   Bool storageDefined) {
        MGPResourceDesc desc{};
        desc.Resource = handle;
        desc.Target = static_cast<Uint8>(kMGPipeResourceTargetBuffer);
        desc.StorageKind = kMGPipeResourceStorageKindBuffer;
        desc.BindMask = bindMask;
        if (storageDefined) {
            desc.Width = static_cast<Uint32>(buffer.GetSize());
            desc.Usage = static_cast<Uint32>(buffer.GetUsage());
            desc.StorageFlags = static_cast<Uint32>(buffer.GetStorageFlags());
            desc.Immutable = buffer.IsImmutableStorage() ? 1 : 0;
            desc.HasDefinedContent = buffer.HasDefinedContent() ? 1 : 0;
        }
        // Diagnostics only: a GL name is never an identity, never a memo key and never part
        // of a content hash (ARCHITECTURE.md 4.2.1).
        desc.GlNameForDiag = static_cast<Uint32>(buffer.GetExternalIndex());
        return desc;
    }

    // The buffer half of MGPSubData: the destination range rides in the box's first
    // coordinate and first extent, and MGPipeSetSubDataBufferRange is the ONLY spelling of
    // that convention. Returns false, with the record untouched, when the range does not fit
    // one record - which is where MGPipeForEachSubDataRecordRange comes in.
    inline Bool MGPipeBuildSubDataRecord(MGPipeHandle res, Uint64 offset, Uint64 size, MGPSubData& out) {
        out = MGPSubData{};
        out.Res = res;
        out.Target = kMGPipeResourceTargetBuffer;
        out.SourceIsVerbatimLevelShadow = 1; // the bytes ARE the client's shadow, unmodified
        if (!MGPipeSetSubDataBufferRange(out, offset, size)) return false;
        out.Blob.Seg = kMGHostSpanSegNone;
        out.Blob.Size = size;
        return true;
    }

    // ONE record caps at a 2^31-1 offset and a 2^32-1 size (MGPipeTypes.h), so a range
    // beyond either has to be split. The pieces are CONTIGUOUS and in ascending order:
    // splitting a content write into overlapping or reordered pieces would change what the
    // backend's queue-and-drain sees, and the Mali WAR-stall fix depends on the queue being
    // exactly the writes the application made.
    //
    // The OFFSET bound cannot be split away - every piece of a range that starts past
    // 2^31-1 starts past it too - so the walk returns false for such a range and emits
    // nothing rather than emitting a record whose box the applier's bounds gate would
    // refuse. That needs a >2 GiB buffer, which nothing in the corpus has; the answer is
    // still stated rather than assumed, because the alternative is a silent truncation.
    template <class Fn>
    inline Bool MGPipeForEachSubDataRecordRange(Uint64 offset, Uint64 size, Fn&& piece) {
        constexpr Uint64 kMaxOffset = 0x7FFFFFFFull;
        constexpr Uint64 kMaxSize = 0xFFFFFFFFull;
        if (offset > kMaxOffset) return false;
        if (size == 0) return true;
        // A single piece may run to the end of the buffer; only its SIZE is split.
        Uint64 at = offset;
        Uint64 left = size;
        while (left > 0) {
            if (at > kMaxOffset) return false;
            const Uint64 chunk = left > kMaxSize ? kMaxSize : left;
            piece(at, chunk);
            at += chunk;
            left -= chunk;
        }
        return true;
    }

    // ---------------------------------------------------------------------------------
    // The tracker: handles, the inverse, the sticky mask, the reverse channel
    // ---------------------------------------------------------------------------------

    class MGPipeResourceTracker {
    public:
        using BufferObject = MG_State::GLState::BufferObject;
        using GLContext = MG_State::GLState::GLContext;

        // The handle for `buffer`, minted on first use. Minting is NOT gated on a backend
        // having registered MGPipeResourceOps: the handle is CLIENT state and
        // set_vertex_buffers names it whether or not the resource family is switched on, so
        // gating it would make the vertex-input subsystem emit null handles whenever the
        // resource subsystem is off. Only the CALLS are gated (D-A1).
        MGPipeHandle Acquire(BufferObject& buffer) {
            const MGPipeHandle handle = MGPipeSlots().Acquire(MGPipeKind::Buffer, buffer.GetLifetimeId());
            const SizeT slot = handle.Slot;
            if (slot >= m_bySlot.size()) m_bySlot.resize(slot + 1);
            m_bySlot[slot].Object = &buffer;
            m_bySlot[slot].Gen = handle.Gen;
            return handle;
        }

        // The handle a buffer already has, or the null handle. Never mints - the emission
        // path calls Acquire, the query paths call this.
        MGPipeHandle Find(const BufferObject& buffer) const {
            return MGPipeSlots().FindByLifetimeId(MGPipeKind::Buffer, buffer.GetLifetimeId());
        }

        // D-D's inverse, and a RAW pointer is exact here: the entry exists only between the
        // create the constructor emits and the destroy the destructor emits, and a readback
        // is only ever issued for a live, bound buffer. A WeakPtr would be wrong - the
        // object does not own itself through a SharedPtr at those two moments. The Gen
        // compare is what refuses a stale handle rather than resolving it to whatever now
        // occupies the slot.
        BufferObject* Resolve(MGPipeHandle handle) const {
            const SizeT slot = handle.Slot;
            if (MGPipeHandleIsNull(handle) || slot >= m_bySlot.size()) return nullptr;
            const Entry& entry = m_bySlot[slot];
            if (entry.Object == nullptr || entry.Gen != handle.Gen) return nullptr;
            if (MGPipeSlots().GenOfSlot(MGPipeKind::Buffer, handle.Slot) != handle.Gen) return nullptr;
            return entry.Object;
        }

        // Drops the inverse entry and the sticky mask. The CALLER frees the slot afterwards,
        // in that order (D-L): MGPipeSlotAllocator::Free erases the lifetimeId -> slot
        // mapping, so anything that has to resolve the handle must do it first.
        void Retire(MGPipeHandle handle) {
            const SizeT slot = handle.Slot;
            if (slot >= m_bySlot.size()) return;
            m_bySlot[slot] = Entry{};
        }

        // The sticky everBoundAs mask. Sticky exactly as MGPResourceDesc::ImageBindableHint's
        // everImageBound is: ORed, never cleared, so a buffer that was an element array once
        // keeps saying so.
        Uint16 BindMask(MGPipeHandle handle) const {
            const SizeT slot = handle.Slot;
            return slot < m_bySlot.size() ? m_bySlot[slot].BindMask : Uint16{0};
        }

        void NoteBoundAs(MGPipeHandle handle, BufferTarget target) {
            const SizeT slot = handle.Slot;
            if (slot >= m_bySlot.size()) return;
            m_bySlot[slot].BindMask |= static_cast<Uint16>(MGPipeBindMaskForBufferTarget(target));
        }

        // Accumulates into the sticky mask every target `buffer` is bound to RIGHT NOW, and
        // returns the accumulated value.
        //
        // [DEVIATION, recorded in client-v1.md] D-A3 asks for the OR at every glBindBuffer /
        // glBindBufferBase / glBindBufferRange / VAO element-slot bind. Those entry points
        // are MG_Impl/GLImpl/Buffer/GL_Buffer.cpp's, which C.5 assigns to no package and
        // C.1 does not list for this one, so the mask is accumulated by SAMPLING the
        // frontend's live binding state instead - here, at every resource emission, which is
        // the only place its value is read. It is still STICKY (the union over every sample
        // this buffer has ever been part of), and it is exact for the GL idiom the bit
        // matters for: bind, then define or update the store. What it cannot see is a bind
        // that happens after the buffer's LAST storage or content operation and is never
        // followed by another - the fix is one line in BindBuffer_State, and it is handed to
        // the integrator rather than taken here.
        //
        // The scan is skipped unless a binding-slot version moved since the last one, which
        // is one Uint16 load per global target and none per binding point.
        Uint16 RefreshBindMask(GLContext& ctx, const BufferObject& buffer, MGPipeHandle handle) {
            const SizeT slot = handle.Slot;
            if (slot >= m_bySlot.size()) return 0;
            Entry& entry = m_bySlot[slot];
            const Uint64 epoch = BindEpoch(ctx);
            if (epoch == m_bindEpoch && entry.BindMaskEpoch == epoch) return entry.BindMask;
            m_bindEpoch = epoch;
            entry.BindMaskEpoch = epoch;
            Uint16 mask = entry.BindMask;
            for (const auto target : MG_State::GLState::GlobalBufferTargets) {
                if (ctx.GetBufferBindingSlot(target).GetBoundObject().get() == &buffer) {
                    mask |= static_cast<Uint16>(MGPipeBindMaskForBufferTarget(target));
                }
            }
            for (const auto target : MG_State::GLState::BufferBindPointTargets) {
                const SizeT touched = ctx.GetTouchedBufferBindingPointCount(target);
                for (SizeT i = 0; i < touched; ++i) {
                    if (ctx.GetBufferBindingPoint(target, static_cast<Uint>(i)).GetBoundObject().get() == &buffer) {
                        mask |= static_cast<Uint16>(MGPipeBindMaskForBufferTarget(target));
                        break;
                    }
                }
            }
            // The index slot is the BOUND VAO's, not BufferState's, so it is not in
            // GlobalBufferTargets and GetBufferBindingSlot(Index) asserts without a VAO.
            if (const auto& vao = ctx.GetBoundVertexArray()) {
                if (vao->GetIndexBufferBindingSlot().GetBoundObject().get() == &buffer) {
                    mask |= static_cast<Uint16>(MGPipeBindMaskForBufferTarget(BufferTarget::Index));
                }
                for (int i = 0; i < MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS; ++i) {
                    if (vao->GetAttribute(static_cast<Uint>(i)).Buffer.get() == &buffer) {
                        mask |= static_cast<Uint16>(MGPipeBindMaskForBufferTarget(BufferTarget::Vertex));
                        break;
                    }
                }
            }
            entry.BindMask = mask;
            return mask;
        }

        // ---- the two observables a unit case reads (see the header comment) ----
        const MGPResourceDesc& LastDesc() const { return m_lastDesc; }
        Uint64 CreateCount() const { return m_creates; }
        Uint64 RespecifyCount() const { return m_respecifies; }
        Uint64 DestroyCount() const { return m_destroys; }
        Uint64 MapPersistentCount() const { return m_mapPersistents; }

        void NoteDesc(const MGPResourceDesc& desc, Bool isCreate) {
            m_lastDesc = desc;
            if (isCreate) {
                ++m_creates;
            } else {
                ++m_respecifies;
            }
        }
        void NoteDestroy() { ++m_destroys; }
        void NoteMapPersistent() { ++m_mapPersistents; }

        // A unit fixture's per-case reset. Never called by the library: a context change
        // does not invalidate a handle, because the handle is the CLIENT's identity for a
        // frontend object that outlives it.
        void ResetForTest() {
            m_bySlot.clear();
            m_bindEpoch = 0;
            m_lastDesc = MGPResourceDesc{};
            m_creates = m_respecifies = m_destroys = m_mapPersistents = 0;
        }

    private:
        struct Entry {
            BufferObject* Object = nullptr;
            Uint32 Gen = 0;
            Uint16 BindMask = 0;
            Uint64 BindMaskEpoch = 0;
        };

        // "Has any buffer binding moved since the last scan": the sum of the binding-slot
        // versions, which BindingSlot bumps only on a real change. A collision costs one
        // skipped rescan of ONE buffer's mask, and the mask is re-scanned at the next
        // emission whose epoch differs, so it can delay a bit by one storage op and never
        // drop one - the same over-fire-is-free / under-fire-is-fatal direction every
        // shutter in Tracker.h takes.
        static Uint64 BindEpoch(GLContext& ctx) {
            Uint64 epoch = 1;
            for (const auto target : MG_State::GLState::GlobalBufferTargets) {
                epoch += ctx.GetBufferBindingSlot(target).GetVersion();
                epoch *= 3;
            }
            if (const auto& vao = ctx.GetBoundVertexArray()) {
                epoch += vao->GetIndexBufferBindingSlot().GetVersion();
                epoch = MGPipeMixShutterValue(epoch, vao->GetLifetimeId());
                epoch = MGPipeMixShutterValue(epoch, vao->GetConfigVersion());
            }
            return epoch;
        }

        // The same mix Tracker.h's composite shutters use. Spelled here rather than
        // included so this header does not depend on the tracker.
        static constexpr Uint64 MGPipeMixShutterValue(Uint64 accumulator, Uint64 value) {
            accumulator ^= value + 0x9e3779b97f4a7c15ull + (accumulator << 6) + (accumulator >> 2);
            return accumulator;
        }

        Vector<Entry> m_bySlot;
        Uint64 m_bindEpoch = 0;
        MGPResourceDesc m_lastDesc{};
        Uint64 m_creates = 0;
        Uint64 m_respecifies = 0;
        Uint64 m_destroys = 0;
        Uint64 m_mapPersistents = 0;
    };

    // The monolith's one resource tracker, beside the state tracker, the CSO cache and the
    // set-hash suppressor.
    inline MGPipeResourceTracker& MGPipeResourceTrackerInstance() {
        static MGPipeResourceTracker tracker;
        return tracker;
    }

    // ---------------------------------------------------------------------------------
    // D-D: the client's half of the reverse channel
    // ---------------------------------------------------------------------------------

    // The backend produced the bytes of a readback and hands them back through the channel.
    // The client resolves the handle to its own object and writes the shadow; the epoch bump
    // stays SERVER-side and happens AFTER this returns, never before (ARCHITECTURE.md 7.4:
    // the reverse channel needs the same ordering guarantee as the forward one).
    inline void MGPipeClientOnBufferWriteback(MGPipeHandle res, Uint64 offset, MGPBlobRef bytes) {
        auto* buffer = MGPipeResourceTrackerInstance().Resolve(res);
        if (buffer == nullptr) {
            MGLOG_E_ONCE("MGPipe: OnBufferWriteback for a handle {%u,%u} that resolves to no buffer",
                         res.Slot, res.Gen);
            return;
        }
        if (bytes.Seg != kMGHostSpanSegNone) {
            MGLOG_E_ONCE("MGPipe: OnBufferWriteback carried a transport segment (%u); P3a is monolith only",
                         bytes.Seg);
            return;
        }
        // Monolith: Seg is kMGHostSpanSegNone and Offset IS the address of the backend's
        // mapped bytes (MGPipeTypes.h says so in as many words). Under a transport the
        // segment resolves first, and that is the phase's edit, not this one's.
        buffer->WritebackFromBackend(
            DataPtr{reinterpret_cast<void*>(static_cast<std::uintptr_t>(bytes.Offset)),
                    static_cast<SizeT>(bytes.Size)},
            static_cast<SizeT>(offset));
    }

    // A draw or dispatch wrote these ranges. ARCHITECTURE.md 7.1 calls this a NARROWING
    // channel - the client builds a conservative pending set at its own emission points and
    // the callback only ever removes from it - so P3a's implementation marks exactly what
    // the three Espryt MarkGpuWritten sites mark today and the observable behaviour is
    // unchanged. The narrowing itself is P8/P9's.
    inline void MGPipeClientOnGpuWritten(MGPipeHandle res, Uint rangeCount, const MGPRange* ranges) {
        (void)rangeCount;
        (void)ranges;
        if (auto* buffer = MGPipeResourceTrackerInstance().Resolve(res)) buffer->MarkGpuWritten();
    }

    // Installed once, and never over an entry a backend already claimed: these two are the
    // CLIENT's implementations of a backend -> frontend callback, so the backend installs
    // the rest of the table and these two answer for it.
    inline void MGPipeInstallClientResourceCallbacks() {
        if (gMGPipeCallbacks.OnBufferWriteback == nullptr) {
            gMGPipeCallbacks.OnBufferWriteback = &MGPipeClientOnBufferWriteback;
        }
        if (gMGPipeCallbacks.OnGpuWritten == nullptr) {
            gMGPipeCallbacks.OnGpuWritten = &MGPipeClientOnGpuWritten;
        }
    }
} // namespace MobileGL::MG_Pipe
#endif // MOBILEGL_PIPE_PUSH
