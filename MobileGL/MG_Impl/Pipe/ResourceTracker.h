// MobileGL - MobileGL/MG_Impl/Pipe/ResourceTracker.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

// Client side of the buffer resource family. Its records go out at the GL call that causes the
// change, from the BufferObject dispatchers that already run the BufferBackendOps hooks, not at
// the validate point (ARCHITECTURE.md §5.1). This file holds the sticky BindMask table, the pure
// record builders and the MGPSubData range splitter, the lifetime id -> {slot, gen} mint, the
// slot -> BufferObject* inverse that writebacks resolve through, and the reverse-channel
// consumers. The MGPipeEmitResource* bodies are in PipeFill.cpp and declared in
// MG_Pipe/PipeMutation.h, so MG_State never includes this file. resource_subdata, the hot path,
// records no test observables; tests drive MGPipeBuildSubDataRecord and the splitter directly.
#include <MG_Impl/Pipe/SlotAllocator.h>
#include <MG_Pipe/MGPipe.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_Pipe/PipeMutation.h>
#include <MG_State/GLState/BufferState/BufferState.h>
#include <MG_State/GLState/Core.h>
#include <MG_Util/Metrics/PipeStats.h>

#include <Config.h>

#if MOBILEGL_BUILD_DISAGGREGATED
// P5c ev (CONTRACT-P5C §4.3): the writeback consumer's SEG_EVENT arm resolves the blobref
// through the client session's own SegmentTable, and the transport check below is the same
// MG_Config::Transport probe PipeFill.cpp uses. Behind the build option for G1's reason -
// nothing under MG_Remote may be reachable from a pull build.
#include <MG_Remote/Client/ClientSession.h>
#endif

#include <cstdint>
#include <cstdlib>

namespace MobileGL::MG_Pipe {

    // ---------------------------------------------------------------------------------
    // D-A3: BindMask
    // ---------------------------------------------------------------------------------

    // MGPResourceDesc::BindMask's twelve bits MOVED TO MG_Pipe/MGPipeTypes.h AT P4a, beside
    // the field, exactly as the note that stood here said they would when a second producer
    // appeared: P4a's texture family sets kMGPipeBindSampler / kMGPipeBindShaderImage /
    // kMGPipeBindRenderTarget / kMGPipeBindDepthStencil, the four bits nothing set before.
    // No alias is written for them because none is possible or needed - both files are
    // namespace MobileGL::MG_Pipe and this one includes that header, so every spelling below
    // and in package B's code is unchanged.
    //
    // What stays here is the BUFFER half of the mapping, which is this file's own: the
    // BufferTarget table, its sentinel and its completeness assert.

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
    // P4a MINTED THE FIRST LIST: MGPipeTypes.h now carries enum MGPipeResourceTarget beside
    // the field, and kMGPipeResourceTargetBuffer moved there with it - the narrowed
    // resource_respecify ack predicate lives in that header and has to name the buffer target
    // explicitly, and it may not reach into MG_Impl to do so. The second discriminator is the
    // frontend enum, named rather than open-coded, and stays here because only this file
    // produces it.
    inline constexpr Uint8 kMGPipeResourceStorageKindBuffer = static_cast<Uint8>(MobileGL::TextureStorageType::Buffer);

    // ---------------------------------------------------------------------------------
    // D-A2: the payload builders. Pure, so a unit case can assert field by field.
    // ---------------------------------------------------------------------------------

    // The descriptor for `buffer`. `storageDefined` is false for the create that the
    // constructor emits - storage is defined lazily by the first respecify and a backend
    // tolerates a resource that has none - and true for every respecify.
    inline MGPResourceDesc MGPipeBuildResourceDesc(const MG_State::GLState::BufferObject& buffer, MGPipeHandle handle,
                                                   Uint16 bindMask, Bool storageDefined) {
        MGPResourceDesc desc{};
        desc.Resource = handle;
        desc.Target = static_cast<Uint8>(kMGPipeResourceTargetBuffer);
        desc.StorageKind = kMGPipeResourceStorageKindBuffer;
        desc.BindMask = bindMask;
        if (storageDefined) {
            // MGPResourceDesc::Width is a Uint32 and that is the CONTRACT's shape, not this
            // package's, so a store of 4 GiB or more cannot be declared at all. Truncating it
            // silently is the one answer that must not happen: the applier's range gate would
            // then refuse the first legal write past the truncated extent as
            // Fatal{ProtocolCorruption} and name a corruption that is really a narrowing here.
            // So it is said out loud, once, in every build - the assertion compiles out at
            // INFO, which is what all three gate builds are.
            if (buffer.GetSize() > static_cast<SizeT>(0xFFFFFFFFull)) {
                MGLOG_E_ONCE("MGPipe: buffer %u declares a store of %llu bytes, which does not fit "
                             "MGPResourceDesc::Width - the descriptor's extent is narrowed and every "
                             "write past 4 GiB will be refused by the applier's range gate",
                             buffer.GetExternalIndex(), static_cast<unsigned long long>(buffer.GetSize()));
                MOBILEGL_ASSERT(false, "MGPResourceDesc::Width cannot carry this buffer's size");
            }
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

    // The buffer half of MGPSubData: the destination range rides in the box's first coordinate
    // and extent, written only through MGPipeSetSubDataBufferRange. Returns false when the range
    // does not fit one record (see MGPipeForEachSubDataRecordRange). The verbatim-shadow flag is
    // a parameter because callers differ: resource_subdata passes the client's own shadow (true),
    // buffer_subdata_resident a staging store or a FillSubData pattern (false). Blob.Seg is
    // kMGHostSpanSegNone (the bytes travel through the entry point's companion pointer) and
    // Blob.Size is the piece's byte length, the only non-zero size SubDataBoxFault accepts.
    inline Bool MGPipeBuildSubDataRecord(MGPipeHandle res, Uint64 offset, Uint64 size, MGPSubData& out,
                                         Bool sourceIsVerbatimLevelShadow) {
        out = MGPSubData{};
        out.Res = res;
        out.Target = kMGPipeResourceTargetBuffer;
        out.SourceIsVerbatimLevelShadow = sourceIsVerbatimLevelShadow ? 1 : 0;
        if (!MGPipeSetSubDataBufferRange(out, offset, size)) return false;
        out.Blob.Seg = kMGHostSpanSegNone;
        out.Blob.Size = size;
        return true;
    }

    // ONE record's destination box caps the offset at 2^31-1 and the size at 2^32-1
    // (MGPipeTypes.h), so a range beyond either has to be split. The pieces are CONTIGUOUS
    // and in ASCENDING order, and both properties are load-bearing rather than tidy:
    // splitting a content write into overlapping or reordered pieces would change what the
    // backend's queue-and-drain sees, and the Mali WAR-stall fix depends on that queue being
    // exactly the writes the application made.
    inline constexpr Uint64 kMGPipeSubDataMaxRecordOffset = 0x7FFFFFFFull;
    inline constexpr Uint64 kMGPipeSubDataMaxRecordSize = 0xFFFFFFFFull;

    // WITH THE RECORD'S OWN BOUND THE SPLIT IS NOT REACHABLE, and saying so is better than a
    // loop that reads as if it were: a second piece starts at least 2^32-1 bytes past the
    // first, which is already past the OFFSET cap, so a range too big for one record is
    // REFUSED rather than split. The offset cap cannot be split away at all - every piece of
    // a range that starts past 2^31-1 starts past it too - and a silent truncation is the one
    // answer that must not happen, so the walk emits nothing and its caller says so once.
    //
    // `maxChunk` exists because the record's bound is not the tight one for long: a transport
    // segment is far smaller (tens of MiB), and that is where this walk starts producing real
    // splits. It is a parameter now, and exercised at a reachable value by the unit gate, so
    // that lowering it is one argument rather than a new code path written under pressure.
    template <class Fn>
    inline Bool MGPipeForEachSubDataRecordRange(Uint64 offset, Uint64 size, Fn&& piece,
                                                Uint64 maxChunk = kMGPipeSubDataMaxRecordSize) {
        if (offset > kMGPipeSubDataMaxRecordOffset) return false;
        if (size == 0) return true;
        if (maxChunk == 0) return false;
        // Every piece has to be encodable BEFORE any of them is emitted: a half-emitted range
        // is a partial content write the backend would land as if it were the whole one.
        const Uint64 chunkCap = maxChunk < kMGPipeSubDataMaxRecordSize ? maxChunk : kMGPipeSubDataMaxRecordSize;
        for (Uint64 at = offset; at < offset + size; at += chunkCap) {
            if (at > kMGPipeSubDataMaxRecordOffset) return false;
        }
        for (Uint64 at = offset, left = size; left > 0;) {
            const Uint64 chunk = left > chunkCap ? chunkCap : left;
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
            const MGPipeHandle handle =
                MGPipeSlots().AcquireHinted(MGPipeKind::Buffer, buffer.GetLifetimeId(), buffer.PipeHandleHint());
            const SizeT slot = handle.Slot;
            if (slot >= m_bySlot.size()) m_bySlot.resize(slot + 1);
            m_bySlot[slot].Object = &buffer;
            m_bySlot[slot].Gen = handle.Gen;
            return handle;
        }

        // The handle a buffer already has, or the null handle. Never mints - the emission
        // path calls Acquire, the query paths call this.
        MGPipeHandle Find(const BufferObject& buffer) const {
            return MGPipeSlots().FindByLifetimeIdHinted(MGPipeKind::Buffer, buffer.GetLifetimeId(),
                                                        buffer.PipeHandleHint());
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

        // ---- D-L: was resource_create actually EMITTED for this slot? ----
        //
        // The create is gated at its call site (BufferObject's constructor) and the destroy
        // is gated inside MGPipeEmitResourceDestroyAndFree, so the two ask the SAME question
        // at two different moments. A buffer constructed while a backend's table was
        // registered and destroyed after UnregisterBufferBackendOps() would take the second
        // answer, free its slot, and leave the applier's record Live - on a slot the
        // allocator is about to hand out again, with the backend's twin (a driver buffer id)
        // still attached to it. So the answer is LATCHED at the create and the destroy uses
        // the latched one; the two are then a pair by construction rather than by the
        // registration outliving every buffer.
        void NotePublished(MGPipeHandle handle) {
            const SizeT slot = handle.Slot;
            if (slot >= m_bySlot.size()) return;
            m_bySlot[slot].Published = true;
        }
        Bool WasPublished(MGPipeHandle handle) const {
            const SizeT slot = handle.Slot;
            return slot < m_bySlot.size() && m_bySlot[slot].Published;
        }
        // A session that replaces an ended one has no record for any buffer the ended one was
        // told about: a live buffer of the ended session's contexts dies with no destroy on the
        // new wire, and the inverse entries stay so the handles still resolve locally.
        void ForgetPublication() {
            for (Entry& entry : m_bySlot)
                entry.Published = false;
        }

        // The sticky everBoundAs mask. Sticky exactly as MGPResourceDesc::ImageBindableHint's
        // everImageBound is: ORed, never cleared, so a buffer that was an element array once
        // keeps saying so.
        Uint16 BindMask(MGPipeHandle handle) const {
            const SizeT slot = handle.Slot;
            return slot < m_bySlot.size() ? m_bySlot[slot].BindMask : Uint16{0};
        }

        // OR one target's bit into a handle's sticky mask, without looking at the context at
        // all. This is what closes the sampling window for the two bits anything keys on:
        // the vertex-input emitters resolve, at EVERY draw, exactly the attribute buffers and
        // the element-slot buffer, so any buffer ever DRAWN FROM carries its ARRAY_BUFFER /
        // ELEMENT_ARRAY bit for the rest of its life whether or not it happened to be bound
        // at a storage op. It grows the table rather than dropping the note: it is called
        // from the validate point, which is GL-thread by construction, and a slot outside the
        // table is a buffer whose mint this process has not seen (a unit fixture's
        // ResetForTest, in practice).
        void NoteBoundAs(MGPipeHandle handle, BufferTarget target) {
            if (MGPipeHandleIsNull(handle)) return;
            const SizeT slot = handle.Slot;
            if (slot >= m_bySlot.size()) m_bySlot.resize(slot + 1);
            m_bySlot[slot].BindMask |= static_cast<Uint16>(MGPipeBindMaskForBufferTarget(target));
        }

        // ORs into the sticky mask every target `buffer` is bound to right now and returns the
        // result. The bind entry points (GL_Buffer.cpp's *_State binders) do not record targets,
        // so the mask is sampled here, at create and respecify where it is published. Sampling
        // misses a transient bind (bind, draw, unbind, then define through DSA); NoteBoundAs at
        // every draw covers the vertex and element-array bits, but other bits of a buffer never
        // drawn from or respecified after its bind stay unpublished. The scan runs only when
        // BindEpoch moved, and never from the content emitters: it reads the whole context, and
        // resource_subdata can run off the render thread.
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

        // Per-case reset for unit fixtures; the library never calls it. Buffer handles and the
        // applier records they name are share-group state: MGPipeApplierReset (a make-current)
        // keeps them, and only the object's own death (resource_destroy from ~BufferObject,
        // delete_vertex_elements) or MGPipeApplierReleaseObjectRecords drops one. So this tracker
        // has no re-publication path on a fresh context; a second resource_create would move the
        // record's Serial for nothing. Per-context latches live in the emitters instead, such as
        // MGPipeVertexInputEmitter::Reset(), which the fresh-context path calls.
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
            Bool Published = false;
            Uint64 BindMaskEpoch = 0;
        };

        // "Has any buffer binding moved since the last scan": the sum of the binding-slot
        // versions, which BindingSlot bumps only on a real change. A collision costs one
        // skipped rescan of ONE buffer's mask, and the mask is re-scanned at the next
        // emission whose epoch differs, so it can delay a bit by one storage op and never
        // drop one - the same over-fire-is-free / under-fire-is-fatal direction every
        // shutter in Tracker.h takes.
        //
        // IT DOES NOT SEE THE 84x4 INDEXED BINDING POINTS, and that is sound only because
        // BindBufferBase_State / BindBufferRange_State also bind the GENERIC slot for the
        // same target (GL_Buffer.cpp:1531 says why), so an indexed bind always moves one of
        // the versions summed here. If that ever stops being true, the CONSTANT /
        // SHADER_BUFFER / ATOMIC / STREAM_OUTPUT bits start being missed silently and the
        // repair is to fold GetTouchedBufferBindingPointCount into the epoch.
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
            const auto avalanche = [](Uint64 v) {
                v ^= v >> 30;
                v *= 0xbf58476d1ce4e5b9ull;
                v ^= v >> 27;
                v *= 0x94d049bb133111ebull;
                v ^= v >> 31;
                return v;
            };
            return avalanche(accumulator ^ avalanche(value + 0x9e3779b97f4a7c15ull));
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
        // NEVER DESTROYED, for MGPipeSlots()' reason (SlotAllocator.cpp): ~BufferObject reads
        // and writes this tracker, and the objects that own the last reference to a
        // BufferObject outlive every function-local static.
        static MGPipeResourceTracker* tracker = new MGPipeResourceTracker();
        return *tracker;
    }

    // ---------------------------------------------------------------------------------
    // D-D: the client's half of the reverse channel
    // ---------------------------------------------------------------------------------

    // Writes the bytes of a backend readback into the client's shadow of the buffer `res` names.
    // The server bumps its epoch only after this returns (ARCHITECTURE.md §8.2). With
    // Seg == kSegEvent, `bytes` is an offset into SEG_EVENT, resolved and bounds-checked through
    // the client session; with Seg == kMGHostSpanSegNone it is a backend address, legal only in
    // the monolith. Anything else, or a host address under an active transport, is
    // Fatal{ProtocolCorruption} (a build without a transport drops it with an error).
    // `producedAtSeq` is the record the server read the bytes back at, 0 for the monolith.
    inline void MGPipeClientOnBufferWritebackAt(MGPipeHandle res, Uint64 offset, MGPBlobRef bytes,
                                                Uint64 producedAtSeq) {
        auto* buffer = MGPipeResourceTrackerInstance().Resolve(res);
        if (buffer == nullptr) {
            MGLOG_E_ONCE("MGPipe: OnBufferWriteback for a handle {%u,%u} that resolves to no buffer", res.Slot,
                         res.Gen);
            return;
        }
        void* bytePtr = nullptr;
        if (bytes.Seg == kMGHostSpanSegNone) {
#if MOBILEGL_BUILD_DISAGGREGATED
            if (MG_Config::Transport != MG_Config::TransportMode::Monolith) {
                MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"OnBufferWriteback.Seg\"} - a "
                        "writeback blobref carried Seg = kMGHostSpanSegNone (a raw host "
                        "address) with an active transport. Rule B binds the reverse "
                        "direction exactly as it binds MGHostSpan: on the wire the blobref "
                        "names SEG_EVENT and an in-segment offset, never a host address");
                std::abort();
            }
#endif
            bytePtr = reinterpret_cast<void*>(static_cast<std::uintptr_t>(bytes.Offset));
        }
#if MOBILEGL_BUILD_DISAGGREGATED
        else if (bytes.Seg == MG_Remote::Wire::kSegEvent) {
            // Size == 0 with a live resource is legal - a zero-length writeback
            // (CONTRACT-P5C §1) - and SegmentTable::Resolve answers nullptr for a zero size,
            // so only a non-empty run goes through the bounds-checked resolve.
            if (bytes.Size != 0) {
                auto* session = MG_Remote::Client::ClientSession::Active();
                const void* resolved = nullptr;
                if (session && session->DataLink()) {
                    session->DataLink()->ResolveSpan(MG_Remote::Transport::LinkSegment::Event,
                                                     {bytes.Offset, bytes.Size}, &resolved);
                }
                if (resolved == nullptr) {
                    MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"OnBufferWriteback.Offset\"} - "
                            "the writeback blobref's {%llu + %llu} does not resolve inside the "
                            "session's SEG_EVENT segment",
                            static_cast<unsigned long long>(bytes.Offset), static_cast<unsigned long long>(bytes.Size));
                    std::abort();
                }
                bytePtr = const_cast<void*>(resolved);
            }
        } else {
            MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"OnBufferWriteback.Seg\"} - a writeback "
                    "blobref carried Seg %u, which is neither kMGHostSpanSegNone (monolith) nor "
                    "kSegEvent (the wire shape)",
                    bytes.Seg);
            std::abort();
        }
#else
        else {
            // The pre-P5c guard's monolith spelling, kept for builds with no transport layer:
            // a segment tag cannot legitimately arrive here and the write must not be made
            // from a null pointer.
            MGLOG_E_ONCE("MGPipe: OnBufferWriteback carried a transport segment (%u) in a build "
                         "with no transport; the writeback is dropped",
                         bytes.Seg);
            return;
        }
#endif
        buffer->WritebackFromBackend(DataPtr{bytePtr, static_cast<SizeT>(bytes.Size)}, static_cast<SizeT>(offset),
                                     producedAtSeq);
    }

    // The monolith callback's shape (MGPipeCallbacks::OnBufferWriteback).
    inline void MGPipeClientOnBufferWriteback(MGPipeHandle res, Uint64 offset, MGPBlobRef bytes) {
        MGPipeClientOnBufferWritebackAt(res, offset, bytes, 0);
    }

    // A draw or dispatch wrote these ranges. ARCHITECTURE.md 7.1 calls this a NARROWING
    // channel - the client builds a conservative pending set at its own emission points and
    // the callback only ever removes from it - so P3a's implementation marks exactly what
    // the three Espryt MarkGpuWritten sites mark today and the observable behavior is
    // unchanged. The narrowing itself is P8/P9's.
    inline void MGPipeClientOnGpuWritten(MGPipeHandle res, Uint rangeCount, const MGPRange* ranges) {
        // THE SHAPE IS A CONTRACT POINT, not a formality: the announcement is ONE range
        // covering kMGPipeWholeBuffer, deliberately not ZERO ranges, because zero will mean
        // "a fully narrowed set - nothing is dirty" at P8/P9. Marking the whole buffer
        // written for a zero-range announcement would be the narrowing channel run backwards,
        // so the shape is asserted here rather than assumed.
        MOBILEGL_ASSERT(rangeCount == 1 && ranges != nullptr,
                        "OnGpuWritten {slot=%u, gen=%u}: P3a announces exactly one whole-buffer range, "
                        "not %u",
                        res.Slot, res.Gen, static_cast<Uint>(rangeCount));
        (void)ranges;
        if (rangeCount == 0) return;
        auto* buffer = MGPipeResourceTrackerInstance().Resolve(res);
        if (buffer == nullptr) {
            // Loud, like its sibling above: a backend announcing a write against a handle
            // this client cannot resolve is a dropped MarkGpuWritten, and a dropped
            // MarkGpuWritten is a stale shadow read back as if it were current.
            MGLOG_E_ONCE("MGPipe: OnGpuWritten for a handle {%u,%u} that resolves to no buffer", res.Slot, res.Gen);
            return;
        }
        buffer->MarkGpuWrittenInChannelOrder();
    }

    // MONOLITH ONLY (CONTRACT-P5C §4.1): with an active transport the three reverse entries
    // of gMGPipeCallbacks are the SERVER session's producer callbacks and the client
    // consumers are invoked BY NAME from DrainEventRing - the global table is a
    // producer-side surface under split. The caller (MGPipeMintResourceHandle) gates on the
    // resolved transport, exactly as PipeFill.cpp's other transport arms do.
    //
    // Installed over only an EMPTY or OUR OWN entry: writing over one somebody else claimed
    // is Fatal{RoleViolation, "callback-double-install"} - the "never over an entry a
    // backend already claimed" comment, made a check. Finding our own function is the
    // idempotent repeat this helper runs once per buffer mint, not a second installation.
    inline void MGPipeInstallClientResourceCallbacks() {
        const auto install = [](auto& entry, auto* fn, const char* name) {
            if (entry != nullptr && entry != fn) {
                MGLOG_F("MGPipe: Fatal{RoleViolation, \"callback-double-install\"} - %s is "
                        "already claimed by a different function. With an active transport "
                        "the reverse entries are the server session's producers; the client "
                        "installs them under monolith only",
                        name);
                std::abort();
            }
            entry = fn;
        };
        install(gMGPipeCallbacks.OnBufferWriteback, &MGPipeClientOnBufferWriteback, "OnBufferWriteback");
        install(gMGPipeCallbacks.OnGpuWritten, &MGPipeClientOnGpuWritten, "OnGpuWritten");
    }

    // Legacy-object entry for the monolith backend. Transport consumers carry handles
    // and call OnGpuWritten directly; P5f fm rejects Magma's P7 buffer consumers before
    // reaching here. Keep a named guard at this boundary so a future caller cannot turn
    // a missing callback/handle into a write through a client object.
    inline void MGPipeAnnounceBufferGpuWritten(const SharedPtr<MG_State::GLState::BufferObject>& bufferObject) {
        if (bufferObject == nullptr) return;
#if MOBILEGL_BUILD_DISAGGREGATED
        if (MG_Config::Transport != MG_Config::TransportMode::Monolith) {
            MGLOG_F("MGPipe: Fatal{RoleViolation, \"gpu-written-legacy-object\"} - "
                    "a transport producer must announce a record handle, never a client BufferObject");
            std::abort();
        }
#endif
        if (gMGPipeCallbacks.OnGpuWritten == nullptr) {
            bufferObject->MarkGpuWritten();
            return;
        }
        const MGPipeHandle res = MGPipeSlots().FindByLifetimeId(MGPipeKind::Buffer, bufferObject->GetLifetimeId());
        if (MGPipeHandleIsNull(res)) {
            MGLOG_E_ONCE("MGPipe: no handle for the GPU-write announcement of buffer %u - the "
                         "reverse channel is installed but the mint is missing, so the mark "
                         "would be dropped at the consumer; poking the object directly rather "
                         "than losing it",
                         bufferObject->GetExternalIndex());
            bufferObject->MarkGpuWritten();
            return;
        }
        const MGPRange whole{0, kMGPipeWholeBuffer};
        gMGPipeCallbacks.OnGpuWritten(res, 1, &whole);
    }
} // namespace MobileGL::MG_Pipe
