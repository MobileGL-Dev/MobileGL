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
                             buffer.GetExternalIndex(),
                             static_cast<unsigned long long>(buffer.GetSize()));
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

    // The buffer half of MGPSubData: the destination range rides in the box's first
    // coordinate and first extent, and MGPipeSetSubDataBufferRange is the ONLY spelling of
    // that convention. Returns false, with the record untouched, when the range does not fit
    // one record - which is where MGPipeForEachSubDataRecordRange comes in.
    //
    // `sourceIsVerbatimLevelShadow` is the record's own question - "are these bytes an
    // untransformed level shadow?" - and it is a PARAMETER because the answer differs by
    // caller: resource_subdata hands over the client's own shadow at an offset into it and
    // says yes; buffer_subdata_resident hands over the application's staging store, or the
    // locally expanded pattern FillSubData built, and both say no. Nothing reads it on the
    // buffer path today, which is exactly why it must not be a hard-coded 1 that becomes
    // wrong the moment something does.
    //
    // Blob is FILLED, exactly: Seg is kMGHostSpanSegNone (monolith - the bytes travel beside
    // the record through the entry point's companion pointer) and Size is the piece's own
    // byte length, which is what the applier's ONE Blob rule holds a non-zero declaration to
    // (PipeApply.cpp's SubDataBoxFault: != 0 && != MGPipeSubDataBufferSize is refused).
    // Leaving it 0 would be legal too; declaring it correctly is the stronger of the two.
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

        // Accumulates into the sticky mask every target `buffer` is bound to RIGHT NOW, and
        // returns the accumulated value.
        //
        // [DEVIATION, recorded in client-v2.md] D-A3 asks for the OR at every glBindBuffer /
        // glBindBufferBase / glBindBufferRange / VAO element-slot bind, and C.1 points at
        // MG_State/GLState/BufferState/BufferState.{h,cpp} for it - a file this package DOES
        // own. The brief is wrong about where the entry points are: BufferState only VENDS
        // BindingSlot<BufferObject>& / BindingSlotRange1D&, and the .Bind() calls are
        // MG_Impl/GLImpl/Buffer/GL_Buffer.cpp's (BindBuffer_State, BindBufferBase_State,
        // BindBufferRange_State), which C.5 assigns to no package. So the mask is accumulated
        // by SAMPLING the frontend's live binding state instead - here, at every create and
        // respecify, which is where the value is PUBLISHED - and ORed into a per-slot sticky
        // field that is never cleared.
        //
        // WHAT SAMPLING ALONE CANNOT SEE is not "a bind after the last respecify" (which the
        // specified design misses too) but a TRANSIENT bind: bind an EBO, draw, unbind, then
        // define it through DSA - the respecify's sample sees no binding at all, and the DSA
        // idiom makes that the common case rather than a corner (TryAdoptLargeStorage's own
        // comment names glNamedBufferSubData as what MC 26.3 streams with). That hole is
        // closed for the two bits anything keys on by NoteBoundAs above, called from
        // EmitVertexBuffers / EmitIndexBuffer at every draw. What is left unpublished is a
        // buffer that is bound, never drawn from, and never re-specified afterwards; the
        // remaining fix is one line in each of GL_Buffer.cpp's three *_State binders, for the
        // seven bits nothing keys on yet, and it stays handed to whoever owns that file.
        //
        // The scan is skipped unless a binding-slot version moved since the last one, which
        // is one Uint16 load per global target and none per binding point. It is NOT called
        // from the content emitters, deliberately: it walks the whole context's binding state
        // and writes the tracker, and one of those emitters (resource_subdata) is on the path
        // D-A2 preserves as reachable off the render thread. Extra sampling could only widen
        // a sticky union, but not at the price of a context-wide read from the wrong thread.
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

        // A unit fixture's per-case reset, and the library never calls it. THE RULE, stated
        // rather than left as an absence, because "nothing resets this" is not a reason:
        //
        //   A buffer handle and the applier record it names are SHARE-GROUP OBJECT STATE.
        //   A GL object lives in a share group, not in a context, so a make-current changes
        //   neither. The applier's MGPipeApplierReset() is a make-current and deliberately
        //   keeps its Resources / VertexElementsCsos (PipeApply.h says so beside them); the
        //   ONLY things that drop a record are the object's own death signal -
        //   resource_destroy, which ~BufferObject raises through
        //   MGPipeEmitResourceDestroyAndFree, and delete_vertex_elements - and
        //   MGPipeApplierReleaseObjectRecords(), which is the SERVED CONTEXT's teardown and
        //   is deliberately wired to nothing in the monolith (there is one applier behind
        //   every context, so calling it on one context's destruction would drop every other
        //   context's records).
        //
        // So this tracker needs no re-publication path on a fresh context and must not have
        // one: re-emitting resource_create for a record the applier still holds would move
        // its Serial for nothing. What the client owes instead is the destroy - which
        // ~BufferObject already emits, in the fixed emit-then-free order (D-L) - and that is
        // the whole of the client's side of the record lifecycle.
        //
        // The vertex-input emitter's latches are the OTHER half and are genuinely per
        // context: MGPipeVertexInputEmitter::Reset() is called from the FreshlyPrimed arm
        // because the applier's vertex-input WORKING state (the bound handle, the window, the
        // fetch shift) IS cleared there. Its vertex-elements RECORDS are not, which is why
        // the emitter's Reset drops the "already published" latches but no create is lost:
        // the latch is what says "re-publish", and re-publishing an unchanged configuration
        // is a bounded over-fire, not a dropped write.
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
        // NEVER DESTROYED, for MGPipeSlots()' reason (SlotAllocator.cpp): ~BufferObject reads
        // and writes this tracker, and the objects that own the last reference to a
        // BufferObject outlive every function-local static.
        static MGPipeResourceTracker* tracker = new MGPipeResourceTracker();
        return *tracker;
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
            MGLOG_E_ONCE("MGPipe: OnGpuWritten for a handle {%u,%u} that resolves to no buffer", res.Slot,
                         res.Gen);
            return;
        }
        buffer->MarkGpuWritten();
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
