// MobileGL - MobileGL/MG_Impl/Pipe/VertexInputEmit.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

// The CLIENT side of P3a's vertex-input family (brief D-G, D-H, D-I): the bound VAO's
// format as create/bind_vertex_elements, its buffers as set_vertex_buffers with an explicit
// baseInstance, and its element binding as set_index_buffer.
//
// UNLIKE THE RESOURCE FAMILY, these three emit at the VALIDATE POINT, from
// MGPipeValidateForVerb's step 3 in the fixed order elements -> buffers -> index. That is
// the ordinary rule (ARCHITECTURE.md 5.1); the resource family is the one exception to it.
//
// THE CSO IS IDENTITY-ADDRESSED, NOT CONTENT-ADDRESSED (D-G1, a recorded deviation from
// ARCHITECTURE.md's 1024-entry content-addressed scheme). One handle per frontend
// VertexArrayObject, minted off its lifetime id, and create_vertex_elements is RE-ISSUED on
// the same handle whenever the configuration moves - legal, because MGPipeHandle::Gen
// increments only on slot reuse and never on a respecify. Espryt has no vertex-elements CSO
// to share: its twin owns one driver VAO name plus 64 scratch buffer ids, which two frontend
// VAOs cannot share, so content addressing would be strictly slower on the only backend this
// phase touches. P7 adds the hash-probe-memcmp layer above these same three calls when
// Magma's VertexInputStateFactory takes the CSO over.
//
// WHAT THE UNIT GATE READS. G6 is "the emitted blob + set + index record reproduce exactly
// what the backend's VAO twin reads from the frontend today, field by field, for all 32
// slots", and G7 is a scripted control that stops the conversion copying ONE field and
// expects the suite to go red NAMING it. So the conversion is a pure function per field
// (MGPipeBuildVertexAttribWire / MGPipeBuildVertexBindingPointWire) and the staging buffers
// the emitter builds into are readable afterwards - the emitter passes m_blob and m_entries
// straight to the applier, so "what was emitted" costs no copy at all.
//
// HEADER-ONLY, for the ownership reason Tracker.h states in full.
#if MOBILEGL_PIPE_PUSH
#include <MG_Impl/Pipe/ResourceTracker.h>
#include <MG_Impl/Pipe/SetHashSuppressor.h>
#include <MG_Impl/Pipe/SlotAllocator.h>
#include <MG_Impl/Pipe/Tracker.h>
#include <MG_Pipe/MGPipe.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_State/GLState/Core.h>
#include <MG_Util/Metrics/PipeStats.h>

#include <xxhash.h>

#include <cstring>

namespace MobileGL::MG_Pipe {

    // ---------------------------------------------------------------------------------
    // D-G2: the wire conversion, one pure function per view
    // ---------------------------------------------------------------------------------

    // EVERY FIELD OF VertexAttribute THE WIRE FORM CARRIES, and nothing else:
    //
    //   Divisor is deliberately absent - it is resolved per binding point and travels in
    //     MGPVertexBuffer::Divisor, which is where the backend's glVertexAttribDivisor reads
    //     it. Carrying it twice would let a malformed record disagree with itself.
    //   LegacyStride / LegacyPointer are deliberately absent - they are the
    //     glGetVertexAttrib* query answers and nothing but the query path reads them, so
    //     they stay client-side.
    //   Buffer is deliberately absent - identity travels in set_vertex_buffers, which is
    //     what keeps this record stable while the buffers under it change.
    //   Stride is the RESOLVED distance and a surviving 0 is MEANINGFUL: a pointer call's 0
    //     was already resolved to the element size by the frontend, so a 0 here can only
    //     have come from the binding model, where it means every vertex reads the SAME
    //     element. Collapsing it back into the element size is what made
    //     KHR-GL43.vertex_attrib_binding.basic-input-case7/8 read past the buffer.
    //   IsLong travels SEPARATELY from Type == Float64: VertexAttribFormat(GL_DOUBLE) reads
    //     doubles and asks for them converted to float, VertexAttribLFormat keeps all 64
    //     bits, and the backend's fp64 narrowing and its Adreno disabled-attribute
    //     workaround both key on telling the two apart.
    inline MGPVertexAttribWire MGPipeBuildVertexAttribWire(const MG_State::GLState::VertexAttribute& attrib,
                                                           Uint32 bindingIndex) {
        // ASSERT RATHER THAN ASSUME, in both directions, because the three narrowing casts
        // below cross a package boundary: VertexArrayObject is another package's file and its
        // 32-slot bound is its invariant, not this one's, so a BindingIndex of 256 would wrap
        // to 0 and silently point every attribute at binding 0, and a negative Stride (the
        // frontend field is a signed int) would arrive as a ~4 GiB unsigned distance.
        MOBILEGL_ASSERT(bindingIndex < 256u,
                        "MGPVertexAttribWire::BindingIndex is a Uint8 and cannot carry %u",
                        static_cast<Uint>(bindingIndex));
        MOBILEGL_ASSERT(attrib.Size >= 0 && attrib.Size <= 255,
                        "MGPVertexAttribWire::Size is a Uint8 and cannot carry %d", attrib.Size);
        MGPVertexAttribWire wire{};
        wire.Offset = static_cast<Uint64>(attrib.Offset);
        wire.Stride = static_cast<Int32>(attrib.Stride);
        wire.Type = static_cast<Uint32>(attrib.Type);
        wire.Size = static_cast<Uint8>(attrib.Size);
        wire.Enabled = attrib.Enabled ? 1 : 0;
        wire.Normalized = attrib.Normalized ? 1 : 0;
        wire.IsInteger = attrib.IsInteger ? 1 : 0;
        wire.IsLong = attrib.IsLong ? 1 : 0;
        wire.IsBgra = attrib.IsBgra ? 1 : 0;
        wire.BindingIndex = static_cast<Uint8>(bindingIndex);
        return wire;
    }

    // The ARB_vertex_attrib_binding view. Its initial Stride is 16, not 0 (GL 4.6 core table
    // 23.4), which is why the wire form keeps it signed and copies it verbatim.
    inline MGPVertexBindingPointWire
    MGPipeBuildVertexBindingPointWire(const MG_State::GLState::VertexBufferBindingPoint& point) {
        MGPVertexBindingPointWire wire{};
        wire.Offset = static_cast<Uint64>(point.Offset);
        wire.Stride = static_cast<Int32>(point.Stride);
        wire.Divisor = static_cast<Uint32>(point.Divisor);
        return wire;
    }

    // ---------------------------------------------------------------------------------
    // D-H2.3: the content hash, WITH BaseInstance in it
    // ---------------------------------------------------------------------------------
    //
    // A HARD REQUIREMENT, not a nicety. set_vertex_buffers is suppressed on an unchanged
    // hash (SetHashSuppressor.h's SetVertexBuffers slot), so a baseInstance that moved while
    // the buffer set did not would be suppressed and the server would keep the previous
    // fetch shift - exactly the bug the backend's baseInstanceDirty flag exists to prevent.
    inline Uint64 MGPipeVertexBufferSetContentHash(const MGPVertexBuffer* entries, Uint32 start, Uint32 count,
                                                   Uint32 baseInstance) {
        Uint64 hash = XXH64(entries, static_cast<SizeT>(count) * sizeof(MGPVertexBuffer), 0);
        hash = MGPipeMixShutter(hash, start);
        hash = MGPipeMixShutter(hash, count);
        hash = MGPipeMixShutter(hash, baseInstance);
        return hash;
    }

    // ---------------------------------------------------------------------------------
    // The emitter
    // ---------------------------------------------------------------------------------

    class MGPipeVertexInputEmitter {
    public:
        using GLContext = MG_State::GLState::GLContext;
        using VertexArrayObject = MG_State::GLState::VertexArrayObject;
        static constexpr SizeT kAttribs = static_cast<SizeT>(VertexArrayObject::MAX_VERTEX_ATTRIBS);
        static constexpr SizeT kBindings = static_cast<SizeT>(VertexArrayObject::MAX_VERTEX_ATTRIB_BINDINGS);
        static_assert(kAttribs <= kMGPipeMaxVertexAttribs && kBindings <= kMGPipeMaxVertexAttribs,
                      "both declared counts are bounded by kMGPipeMaxVertexAttribs");

        // create/bind_vertex_elements. D-G3's three arms, verbatim:
        //
        //   no VAO bound                     -> bind the null handle (legal, and it means
        //                                       exactly "no VAO bound")
        //   the bound VAO CHANGED            -> (re)create if its configuration moved since
        //                                       this handle last published one, then bind
        //   the same VAO, configuration MOVED-> create on the SAME handle, and do NOT rebind
        //
        // The latch is PER HANDLE, in a slot-indexed table, so ping-ponging between two VAOs
        // re-binds but never re-creates either. A Uint32 configuration version does not wrap
        // in any realistic run and is compared directly; the tracker's widened counter is
        // for the Uint16s and is not needed here.
        Uint64 EmitVertexElements(GLContext& ctx) {
            const auto& vao = ctx.GetBoundVertexArray();
            if (!vao) {
                if (!MGPipeHandleIsNull(m_boundHandle)) {
                    MGPipeApplyBindVertexElements(HandleOnly(kMGPipeNullHandle));
                    ++m_binds;
                    m_boundHandle = kMGPipeNullHandle;
                    m_boundLifetimeId = 0;
                }
                return 0;
            }

            const Uint64 lifetimeId = vao->GetLifetimeId();
            const Uint32 configVersion = vao->GetConfigVersion();
            const MGPipeHandle handle = MGPipeSlots().Acquire(MGPipeKind::VertexElementsCso, lifetimeId);
            const SizeT slot = handle.Slot;
            if (slot >= m_latch.size()) m_latch.resize(slot + 1);
            Latch& latch = m_latch[slot];

            Uint64 bytes = 0;
            const Bool configMoved = !latch.Published || latch.ConfigVersion != configVersion ||
                                     latch.Gen != handle.Gen;
            if (configMoved) bytes += EmitCreate(*vao, handle, latch, configVersion);
            if (lifetimeId != m_boundLifetimeId || m_boundHandle != handle) {
                MGPipeApplyBindVertexElements(HandleOnly(handle));
                ++m_binds;
                bytes += sizeof(MGPHandleOnly);
                m_boundHandle = handle;
                m_boundLifetimeId = lifetimeId;
            }
            return bytes;
        }

        // set_vertex_buffers. Espryt consumes RESOLVED attributes, so the set is one entry
        // per attribute slot with BindingIndex == the attribute index; Start is 0 and Count
        // is the highest ENABLED attribute plus one, which is the 32-slot prefix walk the
        // dirty bit is specified over.
        //
        // A client-memory array is Res == kMGPipeNullHandle, and that is not a hole: it is
        // exactly how the server learns "this attribute is client-sourced, upload it
        // yourself". Its store genuinely does not exist at this moment - the client-array
        // uploader runs after PrepareForDraw, at the draw entry point - and moving that
        // resolution to the client is P8's.
        Uint64 EmitVertexBuffers(GLContext& ctx, Uint32 baseInstance) {
            const auto& vao = ctx.GetBoundVertexArray();
            Uint32 count = 0;
            if (vao) {
                for (SizeT i = 0; i < kAttribs; ++i) {
                    if (vao->GetAttribute(static_cast<Uint>(i)).Enabled) count = static_cast<Uint32>(i) + 1;
                }
                for (SizeT i = 0; i < count; ++i) {
                    const auto& attrib = vao->GetAttribute(static_cast<Uint>(i));
                    MGPVertexBuffer& entry = m_entries[i];
                    entry = MGPVertexBuffer{};
                    entry.Res = attrib.Buffer ? MGPipeSlots().Acquire(MGPipeKind::Buffer,
                                                                     attrib.Buffer->GetLifetimeId())
                                              : kMGPipeNullHandle;
                    // D-A3's sticky mask, ORed HERE rather than only sampled at a storage op.
                    // This is the bit that survives the DSA idiom: a buffer defined through
                    // glNamedBuffer* may never be bound at any resource emission, but a draw
                    // that fetches from it resolves it right here, on the GL thread, at every
                    // draw. Sticky, so one draw is enough for the rest of its life.
                    MGPipeResourceTrackerInstance().NoteBoundAs(entry.Res, BufferTarget::Vertex);
                    // The attribute's own byte offset lives in MGPVertexAttribWire::Offset,
                    // so the entry's is the BINDING's, which the frontend already folded in.
                    entry.Offset = 0;
                    // Signed on the frontend, unsigned on the wire, and a negative one would
                    // arrive as a ~4 GiB fetch distance rather than as an error.
                    MOBILEGL_ASSERT(attrib.Stride >= 0, "a resolved vertex stride is never negative (%d)",
                                    attrib.Stride);
                    entry.Stride = static_cast<Uint32>(attrib.Stride);
                    entry.Divisor = static_cast<Uint32>(attrib.Divisor);
                    entry.BindingIndex = static_cast<Uint32>(i);
                }
            }

            const Uint64 hash = MGPipeVertexBufferSetContentHash(m_entries.data(), 0, count, baseInstance);
            if (!MGPipeSetHashSuppressorInstance().ShouldEmit(MGPipeSuppressorSlot::SetVertexBuffers, hash)) {
                return 0;
            }
            m_lastBuffers = MGPVertexBuffers{};
            m_lastBuffers.Start = 0;
            m_lastBuffers.Count = count;
            // THE DRAW'S RAW value. The client never pre-shifts an offset and never learns
            // whether the server emulated the shift or let GL_EXT_base_instance do it -
            // emulation is server-owned.
            m_lastBuffers.BaseInstance = baseInstance;
            m_lastBuffers.ContentHash = hash;
            MGPipeApplySetVertexBuffers(m_lastBuffers, m_entries.data());
            ++m_bufferSets;
            return sizeof(MGPVertexBuffers) + static_cast<Uint64>(count) * sizeof(MGPVertexBuffer);
        }

        // set_index_buffer. An INDEPENDENT call, not a subset of the vertex-elements
        // configuration version (D5) - the index slot is explicitly outside the VAO's
        // m_configVersion, and the shutter for it is bit 10's, narrowed in Tracker.h.
        //
        // Offset and IndexSize are 0 here and the draw verb overrides them: at the validate
        // point there is no draw to read them from, and the applier stores what it is given.
        Uint64 EmitIndexBuffer(GLContext& ctx) {
            const auto& vao = ctx.GetBoundVertexArray();
            m_lastIndex = MGPIndexBuffer{};
            if (vao) {
                if (const auto& bound = vao->GetIndexBufferBindingSlot().GetBoundObject()) {
                    m_lastIndex.Res = MGPipeSlots().Acquire(MGPipeKind::Buffer, bound->GetLifetimeId());
                    // The ELEMENT_ARRAY bit, and it is the one the split path keys on
                    // (kCapNeedsHostIndexBytes -> restart rewriting, multi-draw flattening).
                    // Noted at every draw for RefreshBindMask's reason: an EBO defined through
                    // DSA and unbound before its last respecify would otherwise never publish
                    // it, and getting that bit wrong is invisible in monolith.
                    MGPipeResourceTrackerInstance().NoteBoundAs(m_lastIndex.Res, BufferTarget::Index);
                }
            }
            MGPipeApplySetIndexBuffer(m_lastIndex);
            ++m_indexSets;
            return sizeof(MGPIndexBuffer);
        }

        // ---- what a unit case reads. None of it costs a copy: the emitter builds INTO
        // these and hands the applier the same pointers. ----
        const Array<MGPVertexAttribWire, kMGPipeMaxVertexAttribs>& LastAttributes() const { return m_attributes; }
        const Array<MGPVertexBindingPointWire, kMGPipeMaxVertexAttribs>& LastBindingPoints() const {
            return m_bindingPoints;
        }
        const MGPVertexElements& LastElements() const { return m_lastElements; }
        const MGPVertexBuffers& LastVertexBuffers() const { return m_lastBuffers; }
        const Array<MGPVertexBuffer, kMGPipeMaxVertexAttribs>& LastEntries() const { return m_entries; }
        const MGPIndexBuffer& LastIndexBuffer() const { return m_lastIndex; }
        MGPipeHandle BoundHandle() const { return m_boundHandle; }
        Uint64 CreateCount() const { return m_creates; }
        Uint64 BindCount() const { return m_binds; }
        Uint64 VertexBufferSetCount() const { return m_bufferSets; }
        Uint64 IndexBufferSetCount() const { return m_indexSets; }

        // ---- C-1: "does the applier hold a record for exactly this handle?" ----
        //
        // The CSO's death path (MGPipeEmitVertexElementsDestroyAndFree) needs that answer and
        // MUST NOT GUESS IT FROM THE SLOT. A VertexElementsCso slot can exist with no record
        // behind it, because a backend that keys its twins on the handle mints the slot itself
        // (DirectGLES' BackendSlotTable::GetOrCreate -> MGPipeSlots().Acquire) whether or not
        // bit 8 ever asked this client to emit anything - which is exactly what a
        // MOBILEGL_PIPE_PUSH=0x7f lane runs. delete_vertex_elements on such a handle is a
        // REFUSED call, and the applier's resolver asserts on a refusal
        // (PipeApply.cpp's ResolveVertexElements), i.e. a stop in a verify build.
        //
        // Kept OUT of Reset(), unlike the create/bind latch beside it, and for the mirror
        // image of Reset()'s own reason: "a fresh context is a fresh server" is true of the
        // per-context half of this table, and object RECORDS are precisely what
        // MGPipeApplierReset does not clear (PipeApply.h's two halves). This half tracks those
        // records, so it lives exactly as long as they do.
        Bool RecordIsPublished(MGPipeHandle handle) const {
            if (MGPipeHandleIsNull(handle)) return false;
            const SizeT slot = handle.Slot;
            if (slot >= m_latch.size()) return false;
            const Latch& latch = m_latch[slot];
            return latch.RecordLive && latch.RecordGen == handle.Gen;
        }

        // The record named by `handle` is gone from the applier. Also drops the bound-handle
        // memo when it named it, so the client's idea of BoundVertexElements and the applier's
        // (which MGPipeApplyDeleteVertexElements just cleared for the same handle) stay in
        // step rather than diverging until the next bind happens to correct it.
        void NoteRecordDestroyed(MGPipeHandle handle) {
            if (MGPipeHandleIsNull(handle)) return;
            const SizeT slot = handle.Slot;
            if (slot < m_latch.size() && m_latch[slot].RecordGen == handle.Gen) {
                m_latch[slot] = Latch{};
            }
            if (m_boundHandle == handle) {
                m_boundHandle = kMGPipeNullHandle;
                m_boundLifetimeId = 0;
            }
        }

        // A fresh context is a fresh server: the applier's records are gone, so every latch
        // this emitter holds describes objects the server no longer has. Called from the
        // validate point's FreshlyPrimed arm beside MGPipeApplierReset and the suppressor's
        // InvalidateAll, for the same reason they are.
        //
        // The PER-CONTEXT half only - see RecordIsPublished above for why RecordLive/RecordGen
        // survive. Re-creating a configuration the applier already holds is a bounded
        // over-fire (MGPipeApplyCreateVertexElements starts the record over); forgetting that
        // it holds one at all would leak the record and its slot at the object's death.
        void Reset() {
            for (Latch& latch : m_latch) {
                latch.Published = false;
                latch.Gen = 0;
                latch.ConfigVersion = 0;
            }
            m_boundHandle = kMGPipeNullHandle;
            m_boundLifetimeId = 0;
        }

        void ResetCounters() { m_creates = m_binds = m_bufferSets = m_indexSets = 0; }

    private:
        struct Latch {
            // The PER-CONTEXT half: "has this emitter told THIS server about this handle's
            // configuration". Cleared by Reset() at every make-current.
            Bool Published = false;
            Uint32 Gen = 0;
            Uint32 ConfigVersion = 0;
            // The RECORD half: "does the applier hold a create_vertex_elements record at this
            // slot, for this generation". Lives as long as the record does - see
            // RecordIsPublished.
            Bool RecordLive = false;
            Uint32 RecordGen = 0;
        };

        static MGPHandleOnly HandleOnly(MGPipeHandle handle) {
            MGPHandleOnly only{};
            only.Handle = handle;
            only.Kind = static_cast<Uint32>(MGPipeKind::VertexElementsCso);
            return only;
        }

        Uint64 EmitCreate(const VertexArrayObject& vao, MGPipeHandle handle, Latch& latch, Uint32 configVersion) {
            // ALL 32 OF EACH, deliberately. The record DECLARES both counts and the applier
            // refuses one whose counts do not describe its own blob, so a self-describing
            // record is the cheap shape - and G6 is stated over all 32 slots, which a
            // truncated set could not answer. It rides create_vertex_elements only, i.e.
            // once per configuration change, never per draw.
            for (SizeT i = 0; i < kAttribs; ++i) {
                m_attributes[i] = MGPipeBuildVertexAttribWire(vao.GetAttribute(static_cast<Uint>(i)),
                                                              vao.GetAttributeBindingIndex(static_cast<Uint>(i)));
            }
            for (SizeT i = 0; i < kBindings; ++i) {
                m_bindingPoints[i] = MGPipeBuildVertexBindingPointWire(vao.GetBindingPoint(static_cast<Uint>(i)));
            }
            // Attributes first, then binding points, both ascending and contiguous.
            constexpr SizeT kAttribBytes = kAttribs * sizeof(MGPVertexAttribWire);
            constexpr SizeT kBindingBytes = kBindings * sizeof(MGPVertexBindingPointWire);
            std::memcpy(m_blob.data(), m_attributes.data(), kAttribBytes);
            std::memcpy(m_blob.data() + kAttribBytes, m_bindingPoints.data(), kBindingBytes);

            m_lastElements = MGPVertexElements{};
            m_lastElements.Cso = handle;
            m_lastElements.AttributeCount = static_cast<Uint32>(kAttribs);
            m_lastElements.BindingPointCount = static_cast<Uint32>(kBindings);
            m_lastElements.Blob.Seg = kMGHostSpanSegNone;
            m_lastElements.Blob.Offset = 0;
            m_lastElements.Blob.Size = kAttribBytes + kBindingBytes;
            MGPipeApplyCreateVertexElements(m_lastElements, m_blob.data());
            ++m_creates;
            latch.Published = true;
            latch.Gen = handle.Gen;
            latch.ConfigVersion = configVersion;
            // THE ONE PRODUCER of the record half: a create that reached the applier is the
            // only thing that makes delete_vertex_elements a legal call for this handle.
            latch.RecordLive = true;
            latch.RecordGen = handle.Gen;
            return sizeof(MGPVertexElements) + kAttribBytes + kBindingBytes;
        }

        Array<MGPVertexAttribWire, kMGPipeMaxVertexAttribs> m_attributes{};
        Array<MGPVertexBindingPointWire, kMGPipeMaxVertexAttribs> m_bindingPoints{};
        Array<Uint8, kMGPipeMaxVertexAttribs *(sizeof(MGPVertexAttribWire) + sizeof(MGPVertexBindingPointWire))>
            m_blob{};
        Array<MGPVertexBuffer, kMGPipeMaxVertexAttribs> m_entries{};

        MGPVertexElements m_lastElements{};
        MGPVertexBuffers m_lastBuffers{};
        MGPIndexBuffer m_lastIndex{};

        Vector<Latch> m_latch;
        MGPipeHandle m_boundHandle = kMGPipeNullHandle;
        Uint64 m_boundLifetimeId = 0;

        Uint64 m_creates = 0;
        Uint64 m_binds = 0;
        Uint64 m_bufferSets = 0;
        Uint64 m_indexSets = 0;
    };

    // The monolith's one vertex-input emitter, beside the tracker, the CSO cache, the
    // set-hash suppressor and the resource tracker.
    inline MGPipeVertexInputEmitter& MGPipeVertexInputEmitterInstance() {
        static MGPipeVertexInputEmitter emitter;
        return emitter;
    }
} // namespace MobileGL::MG_Pipe
#endif // MOBILEGL_PIPE_PUSH
