// MobileGL - MobileGL/MG_Backend/DirectGLES/ManagersVertexArrays.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ManagersInternal.h"

namespace MobileGL::MG_Backend::DirectGLES {
    namespace BufferImpl {
        // P13 W4a (§1.4): THE GPU-WRITE REFRESH OF AN ALIASING STORE. Monolith's record arm reads
        // the frontend shadow in place (StagedShadowStore keeps the caller's pointer), so a store a
        // shader wrote is refreshed the way the client refreshes it under split: the bytes are
        // read back from this process's GL buffer and handed to OnBufferWriteback, whose consumer
        // (the frontend, one call away) copies them into that shadow. The backend never writes
        // frontend memory itself. Ops_H_Readback's order, for its reasons: queued uploads first,
        // resident writes next, then the read, the serial stamp and - last - the epoch bump.
        const Uint8* RefreshAliasedStoreFromGpu(GLESBufferResource& resource, MG_Pipe::MGPipeHandle res,
                                                const char* site) {
            if (!CanTouchGLNow() || resource.id == 0 || !resource.storageInitialized || resource.pendingRespecify ||
                resource.contextGeneration != g_bufferContextGeneration || !g_GLESFuncs.glMapBufferRange ||
                !g_GLESFuncs.glUnmapBuffer) {
                MGLOG_E_ONCE("%s: buffer {%u, %u} was written on the GPU but its GL store cannot be read "
                             "back here, so the CPU reads the shadow as it is",
                             site, res.Slot, res.Gen);
                return SplitHostBytes(resource);
            }
            const SizeT width = ResourceWidthOf(res);
            const SizeT size = std::min(width, resource.storageSize);
            if (size == 0) return SplitHostBytes(resource);
            if (MG_Pipe::gMGPipeCallbacks.OnBufferWriteback == nullptr) {
                MGLOG_E_ONCE("%s: buffer {%u, %u} was written on the GPU and no reverse channel is "
                             "installed to refresh the shadow it aliases",
                             site, res.Slot, res.Gen);
                return SplitHostBytes(resource);
            }
            FlushPendingRangesFrom(resource, resource.hostBytes, width);
            DrainResidentWritesNow(resource);
            BindBufferId(TempBufferTarget, resource.id);
            void* mapped =
                g_GLESFuncs.glMapBufferRange(TempBufferTarget, 0, static_cast<GLsizeiptr>(size), GL_MAP_READ_BIT);
            if (mapped == nullptr) {
                MGLOG_E_ONCE("%s: glMapBufferRange(read) failed for GPU-written buffer %u; the CPU reads the "
                             "shadow as it is",
                             site, resource.id);
                return SplitHostBytes(resource);
            }
            MG_Pipe::gMGPipeCallbacks.OnBufferWriteback(res, 0,
                                                        MG_Pipe::MGPBlobRef{reinterpret_cast<Uint64>(mapped),
                                                                            static_cast<Uint64>(size),
                                                                            MG_Pipe::kMGHostSpanSegNone, 0});
            if (resource.hostBytes == nullptr) {
                // Every content-carrying record and every respecify of a store with a size hands
                // its base over on this arm, so no base here means a shape nothing recorded. Keep
                // the bytes in a private copy rather than answer "nothing" for GPU-written bytes.
                MGLOG_W_ONCE("%s: GPU-written buffer {%u, %u} has no recorded shadow base; its refreshed "
                             "bytes are kept in a private copy",
                             site, res.Slot, res.Gen);
                const std::lock_guard<std::mutex> lock(resource.pendingMutex);
                resource.hostBytes = ServerStaged().AdoptPrivateCopy(&resource, width, mapped, size);
            }
            g_GLESFuncs.glUnmapBuffer(TempBufferTarget);
            resource.serverGpuWritten = false;
            resource.syncedChangeSerial = ResourceSerialOf(res);
            BumpBufferMutationEpoch();
            return resource.hostBytes;
        }

        // P8-C. The declaration says what; this says in what order, and it is Ops_H_Readback's
        // order because it is the same question asked by the server of itself instead of by the
        // client: (1) queued uploads land in the GL store first - reading back past them would
        // revert them in the copy - under the same staged-coverage assertion that drain carries;
        // (2) queued resident writes land (GPU-ordered, behind every command already recorded);
        // (3) the whole store is read back into the staged copy, which leaves its coverage whole
        // and so also answers RequireStagedCoverage for an orphaned store's first copy; (4) the
        // serial stamp and the epoch bump, as Ops_H_ReadbackTracked does them.
        const Uint8* SplitHostBytesForCpuRead(GLESBufferResource& resource, MG_Pipe::MGPipeHandle res,
                                              const char* site) {
            // A coherent store - T0's import, or (P13 W4a) a map monolith's record arm donated -
            // has no copy: its bytes are read through the map once queued writes have landed.
            const Bool coherent = resource.persistentMapped && resource.persistentPtr != nullptr;
            Bool residentWritesQueued = false;
            if (coherent) {
                const std::lock_guard<std::mutex> lock(resource.pendingMutex);
                residentWritesQueued = !resource.pendingResidentWrites.empty();
            }
            if (!resource.serverGpuWritten && !residentWritesQueued) return SplitHostBytes(resource);
            if (!ServerStaged().CopiesIntoServerStorage() && !coherent) {
                // P13 W4a (§1.4): an ALIASING store (monolith's record arm) is refreshed through
                // the frontend - the writeback lands in the shadow the base points into. Any other
                // non-copying store has no marks to answer.
                if (MG_Config::RecordArmAliasesFrontend()) return RefreshAliasedStoreFromGpu(resource, res, site);
                return SplitHostBytes(resource);
            }
            if (!CanTouchGLNow() || resource.id == 0 || !resource.storageInitialized || resource.pendingRespecify ||
                resource.contextGeneration != g_bufferContextGeneration || !g_GLESFuncs.glMapBufferRange ||
                !g_GLESFuncs.glUnmapBuffer) {
                MGLOG_E_ONCE("%s: buffer {%u, %u} was written on the GPU but its GL store cannot be read "
                             "back here, so the CPU reads the staged copy as it is",
                             site, res.Slot, res.Gen);
                return SplitHostBytes(resource);
            }
            if (coherent) {
                // T0: the copy IS this server's coherent map of the client's pages. Landing the
                // queued writes and finishing makes them, and every shader write already queued,
                // visible through it - Ops_H_Readback's persistent-map arm, for the same reason.
                DrainResidentWritesNow(resource);
                if (g_GLESFuncs.glFinish) g_GLESFuncs.glFinish();
                resource.serverGpuWritten = false;
                return SplitHostBytes(resource);
            }
            const SizeT width = ResourceWidthOf(res);
            const SizeT size = std::min(width, resource.storageSize);
            if (size == 0) return SplitHostBytes(resource);
            MGL_SERVER_STAGED_REQUIRE_PENDING(resource, resource.hostBytes, width, "gpu_written_refresh_flush_pending");
            FlushPendingRangesFrom(resource, resource.hostBytes, width);
            DrainResidentWritesNow(resource);
            BindBufferId(TempBufferTarget, resource.id);
            void* mapped =
                g_GLESFuncs.glMapBufferRange(TempBufferTarget, 0, static_cast<GLsizeiptr>(size), GL_MAP_READ_BIT);
            if (mapped == nullptr) {
                MGLOG_E_ONCE("%s: glMapBufferRange(read) failed for GPU-written buffer %u; the CPU reads the "
                             "staged copy as it is",
                             site, resource.id);
                return SplitHostBytes(resource);
            }
            {
                const std::lock_guard<std::mutex> lock(resource.pendingMutex);
                resource.hostBytes = MGL_SERVER_STAGED_ADOPT(resource, width, mapped, 0, size);
            }
            g_GLESFuncs.glUnmapBuffer(TempBufferTarget);
            resource.serverGpuWritten = false;
            resource.syncedChangeSerial = ResourceSerialOf(res);
            BumpBufferMutationEpoch();
            MGLOG_D("%s: refreshed the staged copy of GPU-written buffer {%u, %u} (%zu bytes)", site, res.Slot, res.Gen,
                    size);
            return resource.hostBytes;
        }
    } // namespace BufferImpl

    namespace SamplerViewImpl {
        BackendSamplerViewTable g_backendSamplerViews;

        BackendSamplerViewObject* FindSamplerViewForHandle(MG_Pipe::MGPipeHandle view) {
            auto* twin = g_backendSamplerViews.FindByHandle(view);
            return twin ? twin->get() : nullptr;
        }
    } // namespace SamplerViewImpl

    namespace VertexArrayImpl {
        namespace {
            SizeT GetDataTypeSize(DataType type) {
                switch (type) {
                case DataType::Int8:
                case DataType::Uint8:
                    return 1;
                case DataType::Int16:
                case DataType::Uint16:
                case DataType::Float16:
                    return 2;
                case DataType::Int32:
                case DataType::Uint32:
                case DataType::Float32:
                case DataType::Fixed32:
                    return 4;
                case DataType::Float64:
                    return 8;
                default:
                    return 0;
                }
            }

            // Tightly-packed byte size of one vertex element: 4 for the 2_10_10_10 types and GL_BGRA
            // (one 32-bit word / 4 bytes), componentSize * size otherwise. 0 for unknown types.
            SizeT GetAttributeByteSize(DataType type, int size, Bool isBgra) {
                if (type == DataType::Int2101010Rev || type == DataType::Uint2101010Rev || isBgra) {
                    return 4;
                }
                const SizeT componentSize = GetDataTypeSize(type);
                return componentSize == 0 ? 0 : componentSize * static_cast<SizeT>(size);
            }

            // Deinterleaves elementCount elements of componentCount doubles into a tightly packed
            // float32 stream - the fetch half of the fp64 demotion the shader side already does
            // unconditionally (DemoteFloat64Pass). GL byte strides and offsets are arbitrary, so
            // no component carries an 8-byte alignment guarantee and each is copied out before it
            // is narrowed.
            void NarrowDoubleStreamToFloat32(const Uint8* sourceBase, SizeT sourceStride, SizeT componentCount,
                                             SizeT elementCount, Vector<Float>& outData) {
                outData.resize(elementCount * componentCount);
                for (SizeT element = 0; element < elementCount; ++element) {
                    const Uint8* sourceElement = sourceBase + element * sourceStride;
                    Float* destinationElement = outData.data() + element * componentCount;
                    for (SizeT component = 0; component < componentCount; ++component) {
                        Double value = 0.0;
                        Memcpy(&value, sourceElement + component * sizeof(Double), sizeof(Double));
                        destinationElement[component] = static_cast<Float>(value);
                    }
                }
            }

            // GL_BGRA as a vertex SIZE is desktop-only - ES rejects it outright - and it means
            // nothing more than "the element's first and third components are stored swapped".
            // So the element is put back in RGBA order and fetched as an ordinary size-4 array of
            // the same type: for GL_UNSIGNED_BYTE the first and third BYTES swap, for the two
            // 2_10_10_10_REV types the 10-bit fields in bits 0-9 and 20-29 (the core spec's packed
            // component layout: with BGRA, x comes from bits 20-29 and z from bits 0-9). The 2-bit
            // w field and the middle field stay where they are. Elements are copied word-wise, so
            // no source alignment is assumed; the output is tightly packed, one word per element.
            void SwizzleBgraElementToRgba(Uint8* element, DataType type) {
                if (type == DataType::Uint8) {
                    std::swap(element[0], element[2]);
                    return;
                }
                Uint32 word = 0;
                Memcpy(&word, element, sizeof(word));
                constexpr Uint32 kField = 0x3FFu;
                const Uint32 low = word & kField;
                const Uint32 high = (word >> 20) & kField;
                word = (word & ~(kField | (kField << 20))) | high | (low << 20);
                Memcpy(element, &word, sizeof(word));
            }

            void SwizzleBgraStreamToRgba(const Uint8* sourceBase, SizeT sourceStride, SizeT elementCount, DataType type,
                                         Vector<Uint8>& outData) {
                outData.resize(elementCount * 4);
                for (SizeT element = 0; element < elementCount; ++element) {
                    Uint8* destination = outData.data() + element * 4;
                    Memcpy(destination, sourceBase + element * sourceStride, 4);
                    SwizzleBgraElementToRgba(destination, type);
                }
            }
        } // namespace

        BackendVertexArrayObject::BackendVertexArrayObject() {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            m_clientAttributeBufferIds.fill(0);
            m_convertedAttributeBufferIds.fill(0);
            g_GLESFuncs.glGenVertexArrays(1, &m_backendVAOId);
            if (m_backendVAOId == 0) {
                MGLOG_E_ONCE("Failed to generate vertex array object.");
                MGLOG_E_ONCE("ES glGetError(): %s", MG_Util::ConvertGLEnumToString(g_GLESFuncs.glGetError()).c_str());
            } else {
                MGLOG_D("Generated vertex array object with ID: %u.", m_backendVAOId);
            }
        }

        BackendVertexArrayObject::~BackendVertexArrayObject() {
            if (InProcessTeardown()) {
                return; // see InProcessTeardown(): the driver may be unloaded already
            }
            if (m_backendVAOId != 0) {
                NoteVAOIdDeleted(m_backendVAOId);
                g_GLESFuncs.glDeleteVertexArrays(1, &m_backendVAOId);
                m_backendVAOId = 0;
            }
            for (auto& bufferId : m_clientAttributeBufferIds) {
                if (bufferId != 0) {
                    BufferImpl::NoteBufferIdDeleted(bufferId);
                    g_GLESFuncs.glDeleteBuffers(1, &bufferId);
                    bufferId = 0;
                }
            }
            for (auto& bufferId : m_convertedAttributeBufferIds) {
                if (bufferId != 0) {
                    BufferImpl::NoteBufferIdDeleted(bufferId);
                    g_GLESFuncs.glDeleteBuffers(1, &bufferId);
                    bufferId = 0;
                }
            }
        }

        namespace {
            Uint g_boundBackendVAOId = 0;
            Bool g_boundBackendVAOKnown = false;
        } // namespace

        void BindBackendVAOId(Uint id) {
            if (g_boundBackendVAOKnown && g_boundBackendVAOId == id) {
                return;
            }
            g_GLESFuncs.glBindVertexArray(id);
            g_boundBackendVAOId = id;
            g_boundBackendVAOKnown = true;
        }

        void InvalidateVAOBindingCache() {
            g_boundBackendVAOKnown = false;
        }

        void NoteVAOIdDeleted(Uint id) {
            if (g_boundBackendVAOKnown && g_boundBackendVAOId == id) {
                g_boundBackendVAOId = 0; // glDeleteVertexArrays reverts a bound VAO to 0
            }
        }

        void BackendVertexArrayObject::Bind() const {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            BindBackendVAOId(m_backendVAOId);
        }

        // P15: the driver-VAO shadow (Managers.h). Callers have bound this twin's VAO.

        namespace {
            inline Bool VaoShadowOn() {
                return MG_Config::Features.EsprytVaoShadow != MG_Config::QuirkOverride::ForceOff;
            }
        } // namespace

        void BackendVertexArrayObject::HostSetEnabled(Uint index, Bool enabled) {
            const Uint32 bit = 1u << index;
            if (VaoShadowOn() && (m_hostEnabledKnownMask & bit) && ((m_hostEnabledMask & bit) != 0) == enabled) return;
            if (enabled)
                g_GLESFuncs.glEnableVertexAttribArray(index);
            else
                g_GLESFuncs.glDisableVertexAttribArray(index);
            m_hostEnabledKnownMask |= bit;
            m_hostEnabledMask = enabled ? (m_hostEnabledMask | bit) : (m_hostEnabledMask & ~bit);
        }

        void BackendVertexArrayObject::HostSetDivisor(Uint index, Uint32 divisor) {
            const Uint32 bit = 1u << index;
            if (VaoShadowOn() && (m_hostDivisorKnownMask & bit) && m_hostDivisor[index] == divisor) return;
            g_GLESFuncs.glVertexAttribDivisor(index, divisor);
            m_hostDivisorKnownMask |= bit;
            m_hostDivisor[index] = divisor;
        }

        void BackendVertexArrayObject::HostForgetAll() {
            m_hostEnabledKnownMask = 0;
            m_hostDivisorKnownMask = 0;
            for (auto& pointer : m_hostPointer)
                pointer.valid = false;
        }

        // ES 3.1 core. Queried through the loader rather than the version, because the whole
        // point of using it is to express something the pointer API cannot, and falling back
        // silently on a driver that lacks it is better than crashing on a null entry point.
        inline Bool HasVertexBindingApi() {
            return g_GLESFuncs.glBindVertexBuffer != nullptr && g_GLESFuncs.glVertexAttribFormat != nullptr &&
                   g_GLESFuncs.glVertexAttribIFormat != nullptr && g_GLESFuncs.glVertexAttribBinding != nullptr &&
                   g_GLESFuncs.glVertexBindingDivisor != nullptr;
        }

        Bool BackendUsesNativeBaseInstance() {
            return g_GLESCapabilities.SupportsBaseInstance;
        }

        // The "+ baseInstance" of GL's instanced-array element index, expressed as a byte shift
        // of the array's own offset. Only divisor'd arrays step per instance, so only they move.
        //
        // baseInstance is added to the ELEMENT index, not to instance/divisor - the divisor
        // therefore does not appear here, and the shift is a whole number of strides.
        //
        // A resolved stride of zero is the binding model's "never advance" (see
        // VertexAttribute::Stride), so such an array reads the same element for every instance
        // and a baseInstance cannot move it. The arithmetic already yields zero for that case.
        inline SizeT BaseInstanceByteShift(const MG_State::GLState::VertexAttribute& attrib, Uint32 baseInstance) {
            if (baseInstance == 0 || attrib.Divisor == 0) {
                return 0;
            }
            return static_cast<SizeT>(baseInstance) * static_cast<SizeT>(attrib.Stride);
        }

        // ---- the handle arm's small helpers -------------------------------------------
        //
        // Each one is its object-shaped counterpart above with the frontend reads replaced by
        // the two wire views, and nothing else. Divisor is deliberately NOT on the attribute
        // view: it is resolved per binding point and rides in MGPVertexBuffer::Divisor, which
        // is where glVertexAttribDivisor reads it (D-G2).

        // The entry of the applied set that feeds ATTRIBUTE `attributeIndex`.
        //
        // THE KEY IS THE ATTRIBUTE INDEX, NOT MGPVertexAttribWire::BindingIndex, and the two
        // live in different spaces. Espryt consumes RESOLVED attributes, so the client emits
        // set_vertex_buffers as one entry per attribute slot with
        // MGPVertexBuffer::BindingIndex == the attribute index (EmitVertexBuffers in VertexInputEmit.h -
        // "Espryt consumes RESOLVED attributes, so the set is one entry per attribute
        // slot with BindingIndex == the attribute index"), each carrying that attribute's
        // already-folded buffer, stride and divisor. MGPVertexAttribWire::BindingIndex is the
        // OTHER thing: the GL binding POINT the attribute was attached to by
        // glVertexAttribBinding, which is the key of the binding-point view this arm never
        // reads (it needs no separate view, because the resolution already happened on the
        // client). The two coincide whenever the attribute was configured through
        // glVertexAttribPointer, which is why every scenario that uses the pointer API stayed
        // green while glVertexAttribBinding(2, 3) / (0, 5) fetched the wrong entry or none -
        // KHR-GL43.vertex_attrib_binding's whole subject.
        const MG_Pipe::MGPVertexBuffer* VertexBufferForAttributeIndex(const MG_Pipe::MGPipeApplierState& st,
                                                                      Uint32 attributeIndex) {
            if (st.VertexBufferCount == 0) return nullptr;
            const Uint32 begin = st.VertexBufferStart;
            const Uint32 end = begin + st.VertexBufferCount;
            if (attributeIndex >= begin && attributeIndex < end && attributeIndex < MG_Pipe::kMGPipeMaxVertexAttribs &&
                st.VertexBuffers[attributeIndex].BindingIndex == attributeIndex) {
                return &st.VertexBuffers[attributeIndex];
            }
            for (Uint32 i = begin; i < end && i < MG_Pipe::kMGPipeMaxVertexAttribs; ++i) {
                if (st.VertexBuffers[i].BindingIndex == attributeIndex) return &st.VertexBuffers[i];
            }
            return nullptr;
        }

        // BaseInstanceByteShift, on the wire views. baseInstance is added to the ELEMENT index,
        // so the divisor does not appear here; a resolved stride of zero never advances and the
        // arithmetic already yields zero for it.
        inline SizeT BaseInstanceByteShiftWire(Int32 stride, Uint32 divisor, Uint32 baseInstance) {
            if (baseInstance == 0 || divisor == 0) return 0;
            return static_cast<SizeT>(baseInstance) * static_cast<SizeT>(stride);
        }

        // (The attribute-buffer bind lives in SyncToBackendFromApplier's pointer path now: it
        // resolves the driver id from the slot table and skips the bind and the pointer when the
        // driver VAO already holds them. It does NOT ensure storage: on this arm the draw's
        // buffers were ensured by SyncNeccessaryBuffers earlier in the same PrepareForDraw.)

        // SyncZeroStrideAttribute on the wire views: the one spelling that can carry a resolved
        // stride of zero, which glVertexAttribPointer's zero means the opposite of.
        inline Bool SyncZeroStrideAttributeByHandle(Uint attribIndex, const MGPVertexAttribWire& attrib,
                                                    const MG_Pipe::MGPVertexBuffer& binding, GLenum issuedType) {
            if (MG_Pipe::MGPipeHandleIsNull(binding.Res)) {
                MGLOG_W_ONCE("Zero-stride attribute %u has no bound buffer, skipping.", attribIndex);
                return false;
            }
            auto* backendResource = BufferImpl::FindBufferResourceForHandle(binding.Res);
            if (!backendResource || backendResource->id == 0) {
                MGLOG_E_ONCE("No backend buffer for zero-stride attribute %u, cannot bind it.", attribIndex);
                return false;
            }
            if (!attrib.IsInteger) {
                const GLint glSize = static_cast<GLint>(attrib.Size);
                g_GLESFuncs.glVertexAttribFormat(attribIndex, glSize,
                                                 MG_Util::ConvertDataTypeToGLEnum(static_cast<DataType>(attrib.Type)),
                                                 attrib.Normalized ? GL_TRUE : GL_FALSE, 0);
            } else {
                g_GLESFuncs.glVertexAttribIFormat(attribIndex, static_cast<GLint>(attrib.Size), issuedType, 0);
            }
            g_GLESFuncs.glVertexAttribBinding(attribIndex, attribIndex);
            // The resolved offset goes on the binding point, not into a relative offset (the
            // relative offset is capped by GL_MAX_VERTEX_ATTRIB_RELATIVE_OFFSET, a buffer
            // offset is not). BindBufferId is bypassed deliberately.
            g_GLESFuncs.glBindVertexBuffer(attribIndex, backendResource->id, static_cast<GLintptr>(attrib.Offset), 0);
            return true;
        }

        // The record the applier holds for the bound vertex-elements CSO, or null.
        const MG_Pipe::MGPipeVertexElementsRecord* BoundVertexElementsRecord(const MG_Pipe::MGPipeApplierState& st) {
            const MG_Pipe::MGPipeHandle cso = st.BoundVertexElements;
            if (MG_Pipe::MGPipeHandleIsNull(cso)) return nullptr;
            if (cso.Slot >= st.VertexElementsCsos.size()) return nullptr;
            const auto& record = st.VertexElementsCsos[cso.Slot];
            if (!record.Live || record.Gen != cso.Gen) return nullptr;
            return &record;
        }

        // P5e (id), CONTRACT-P5E §4.1. See Managers.h for the contract; the body is the one
        // SamplerImpl::ResolveSamplerCsoTwin already runs, with the sync left to vi.
        BackendVertexArrayObject* ResolveVaoTwin(MG_Pipe::MGPipeHandle elements) {
            if (MG_Pipe::MGPipeHandleIsNull(elements)) return nullptr;
            // THE RECORD FIRST, before the table is touched: a handle with no record means the
            // client named a vertex-elements CSO it never described (or one it deleted while a
            // binding still names it), and adopting a slot for it would leave a twin nothing
            // can ever sync. The applier's own Gen check is what turns a stale handle into this
            // null rather than into its successor's record.
            if (PipeRecordAt(MG_Pipe::MGPipeApplier().VertexElementsCsos, elements) == nullptr) {
                MGLOG_E_ONCE("MGPipe: vertex-elements CSO {%u, %u} has no applier record on the handle "
                             "arm, so no driver VAO can be built for it and the draw keeps what is bound",
                             elements.Slot, elements.Gen);
                return nullptr;
            }
            auto* slot = AdoptTwinByHandle(g_backendVertexArrayObjects, elements, "vertex-elements CSO");
            if (slot == nullptr) return nullptr;
            if (!*slot) *slot = MakeShared<BackendVertexArrayObject>();
            return slot->get();
        }

        // The same function, driven by the applier's records (D-G4). Every branch of the walk
        // survives - the enable/disable block, the fp64 narrowing with its Adreno disable, the
        // zero-stride binding-API path, the attribute-buffer bind, the BGRA swizzle, the
        // pointer/IPointer at the shifted fetch offset and the divisor - and the gate is
        // re-keyed onto three server-owned MGGen counters plus the CSO's {slot, gen}:
        //
        //   m_syncedElementsHandle + m_syncedElementsSerial   <- config version + the whole
        //                                                        per-attribute version array
        //   m_syncedVertexBuffersSerial                       <- (new) the buffer set's own
        //   m_syncedIndexSerial                               <- wrapping Uint16 + identity patch
        //   m_syncedBufferIdGeneration                        <- UNCHANGED, server-local, and
        //                                                        still the only thing that
        //                                                        catches a driver-id re-mint no
        //                                                        counter on either side moves
        //   m_hasConvertedAttribute                           <- UNCHANGED: a narrowed stream is
        //                                                        derived from buffer CONTENT,
        //                                                        which no serial covers
        //   m_syncedFetchBaseInstance                         <- no longer in the gate as a
        //                                                        DIRTY input of its own (a base
        //                                                        instance change is a
        //                                                        ContentHash input and so moves
        //                                                        VertexBuffersSerial), but kept
        //                                                        as the record of what was last
        //                                                        EMITTED, which is what the next
        //                                                        sync has to correct
        void BackendVertexArrayObject::SyncToBackendFromApplier(const IntegerAttributePlan& plan) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            const MG_Pipe::MGPipeApplierState& st = MG_Pipe::MGPipeApplier();
            const auto* rec = BoundVertexElementsRecord(st);
            if (rec == nullptr) {
                MGLOG_E_ONCE("MGPipe: no vertex-elements record is bound, so the driver VAO cannot be "
                             "configured - nothing emitted create_vertex_elements/bind_vertex_elements");
                return;
            }

            const Uint64 currentBufferIdGeneration = BufferImpl::g_bufferBackendIdGeneration;
            const Bool bufferIdsRemitted = m_syncedBufferIdGeneration != currentBufferIdGeneration;
            // THIS GATE DEPENDS ON A SERIAL RULE THAT LIVES IN ANOTHER PACKAGE, and it is named
            // here because nothing else in this file would say it: MGPipeApplierReset() runs on
            // EVERY change of the current GLContext (MG_Impl/Pipe/Tracker.h's `if (m_context !=
            // &ctx) Reset();`, not only on a fresh one), while this twin SURVIVES the excursion -
            // BackendVertexArrayObject has no context-generation member and
            // OnBackendContextDestroyed runs on destroy, not on make-current. A reset that sent
            // VertexBuffersSerial and IndexBufferSerial back to ZERO would therefore walk them
            // back through values this twin has already stamped, and a memo could read clean over
            // state the applier had just cleared. MG_Pipe/PipeApply.cpp's reset must ADVANCE
            // those two serials instead (wire's C2), which is what makes
            // m_syncedVertexBuffersSerial below unable to match after a reset - and that in turn
            // forces the whole AND dirty, which is also what rescues the per-record ContentSerial
            // half (an applier reset is not a slot recycle, so a re-created CSO at the same
            // {slot, gen} restarts its ContentSerial at 1). If that rule is ever reverted, this
            // gate is unsafe on any application that changes contexts.
            const Bool elementsDirty = bufferIdsRemitted || !m_hasSyncedElements ||
                                       !(m_syncedElementsHandle == st.BoundVertexElements) ||
                                       m_syncedElementsSerial != rec->ContentSerial;
            const Bool attributesDirty = elementsDirty || m_syncedVertexBuffersSerial != st.VertexBuffersSerial;
            const Bool indexBufferDirty = bufferIdsRemitted || m_syncedIndexSerial != st.IndexBufferSerial;
            // Emulation is server-owned: the client sends the draw's RAW base instance and never
            // learns the answer. Applied here as well as in the applier so the decision is the
            // same whichever side resolved it first - it is idempotent.
            const Uint32 fetchBaseInstance = BackendUsesNativeBaseInstance() ? 0u : st.VertexFetchBaseInstance;
            const Bool baseInstanceDirty = m_syncedFetchBaseInstance != fetchBaseInstance;
            // The integer arrays to issue with the other signedness; only a pointer whose type flips
            // is re-issued, through the shadow below.
            const Uint32 retypeHeld = HeldIntegerRetype(m_syncedRetypeMask, plan);
            const Bool emitAttributes =
                attributesDirty || baseInstanceDirty || m_hasConvertedAttribute || retypeHeld != m_syncedRetypeMask;
            if (!emitAttributes && !indexBufferDirty) {
                return;
            }
            // P14: ONLY THE BUFFER SET MOVED (a Minecraft chunk draw: same layout, next chunk's
            // buffer). The driver VAO's enable flags are a function of the configuration alone,
            // and the last full walk left them exactly as this configuration wants them, so the
            // walk below re-points just the enabled attributes and skips the 32 enable/disable
            // calls. Only after a walk in which every enabled attribute took the plain pointer
            // path: the fp64, zero-stride, BGRA and no-buffer branches toggle enables or bind
            // through other state, and the client-memory upload rewrites the VAO behind this
            // gate, so any of them sends the next sync through the whole walk again.
            const Bool buffersOnly = emitAttributes && !elementsDirty && m_lastWalkWasPlain && !m_hasConvertedAttribute;
            Bool walkWasPlain = true;
            m_hasConvertedAttribute = false;
            // Rebuilt by every walk; a buffers-only one follows a plain walk, which set none.
            if (emitAttributes && !buffersOnly) m_disabledEnabledAttribMask = 0;

            Bind();
            if (m_hostPointerBufferGeneration != currentBufferIdGeneration) {
                for (auto& pointer : m_hostPointer)
                    pointer.valid = false;
                m_hostPointerBufferGeneration = currentBufferIdGeneration;
            }

            // ALL 32 SLOTS, not rec->AttributeCount, and the difference is the disable arm.
            // The legacy walk ran over the frontend's whole 32-slot attribute array and reached
            // glDisableVertexAttribArray for every attribute that was off; a walk bounded by the
            // record's count leaves an attribute the configuration has just DROPPED enabled in
            // the driver VAO, pointing at whatever buffer it last held - which is the class the
            // first Adreno workaround below exists to prevent. Nothing in the contract requires
            // the client to emit 32 entries (D-H3 sets the precedent the other way for the
            // BUFFER set: "truncated to the highest enabled attribute + 1"), so the bound was a
            // requirement on B that was neither stated nor pinned. It costs nothing to drop it:
            // MGPipeApplyCreateVertexElements zeroes both arrays before it unpacks, so every
            // entry past AttributeCount reads Enabled = 0, Type = 0 (Int8, not Float64) and
            // IsLong = 0 - i.e. exactly "disable this array", with no dependency on B at all.
            for (Uint attribIndex = 0; attribIndex < MG_Pipe::kMGPipeMaxVertexAttribs && emitAttributes;
                 ++attribIndex) {
                const MGPVertexAttribWire& attrib = rec->Attributes[attribIndex];
                if (buffersOnly && !attrib.Enabled) continue;
                const MG_Pipe::MGPVertexBuffer* binding = VertexBufferForAttributeIndex(st, attribIndex);
                const Uint32 divisor = binding != nullptr ? binding->Divisor : 0u;

                // The enable/disable block. On this arm there is no per-attribute version to
                // compare: the applier's Attributes[] IS what was last pushed, so a moved
                // ContentSerial means re-emit and an unchanged one means the early-out above
                // already returned. A buffers-only walk leaves the flags as they are (above).
                if (!buffersOnly) HostSetEnabled(attribIndex, attrib.Enabled != 0);

                // The fp64 narrowing, verbatim in behaviour including the Adreno workaround:
                // when no float32 stream can be built the array is DISABLED rather than left
                // enabled with no pointer, which is what the driver turns into a SIGSEGV inside
                // the next draw (KHR-GL43.vertex_attrib_binding.basic-input-case4). IsLong is
                // carried separately from Type == Float64 on the wire precisely so this test
                // can still tell the two apart.
                //
                // A GL_BGRA-sized array takes the same stream, swizzled instead of narrowed, and
                // the same disable when it cannot be built: GL_BGRA as a vertex SIZE is
                // desktop-only, ES rejects it, and the rejected call leaves the array ENABLED
                // with no pointer - the shape the Adreno driver dereferences null on inside the
                // next draw (KHR-GL43.vertex_attrib_binding.basic-input-case5).
                const Bool bgraArray = attrib.IsBgra != 0 && !attrib.IsInteger;
                if (attrib.IsLong || attrib.Type == static_cast<Uint32>(DataType::Float64) || bgraArray) {
                    walkWasPlain = false;
                    const Bool convertible = bgraArray || attrib.Type == static_cast<Uint32>(DataType::Float64);
                    if (attrib.Enabled && convertible && binding != nullptr &&
                        SyncConvertedAttributeByHandle(attribIndex, attrib, *binding, fetchBaseInstance)) {
                        m_hasConvertedAttribute = true;
                        // The conversion pointed the attribute at its own stream (and may have
                        // used the binding API), so the pointer and divisor shadow are stale.
                        // An earlier walk that could not build the stream disabled this array.
                        m_hostPointer[attribIndex].valid = false;
                        m_hostDivisorKnownMask &= ~(1u << attribIndex);
                        HostSetEnabled(attribIndex, true);
                        HostSetDivisor(attribIndex, divisor);
                        continue;
                    }
                    if (attrib.Enabled) {
                        MGLOG_W_ONCE("DirectGLES: vertex attribute %u is a %s array whose source stream could not "
                                     "be converted - disabling the array",
                                     attribIndex, bgraArray ? "GL_BGRA" : "64-bit (GL_DOUBLE)");
                        // The draw now reads the attribute's current generic value, which the
                        // application had no reason to set for an enabled array.
                        m_disabledEnabledAttribMask |= 1u << attribIndex;
                    }
                    HostSetEnabled(attribIndex, false);
                    continue;
                }

                if (!attrib.Enabled) continue;
                if (binding == nullptr) {
                    walkWasPlain = false;
                    continue;
                }

                // A resolved stride of zero is the binding model's "never advance" and
                // glVertexAttribPointer cannot say it - its zero means "tightly packed", i.e.
                // the opposite. ES 3.1's binding-point API can.
                if (attrib.Stride == 0 && HasVertexBindingApi()) {
                    walkWasPlain = false;
                    // The binding API rewrote this attribute's format and binding: whatever the
                    // shadow said about its pointer no longer holds.
                    m_hostPointer[attribIndex].valid = false;
                    const GLenum issuedType =
                        IntegerAttribTypeForInput(MG_Util::ConvertDataTypeToGLEnum(static_cast<DataType>(attrib.Type)),
                                                  attrib.IsInteger && ((retypeHeld >> attribIndex) & 1u));
                    if (!SyncZeroStrideAttributeByHandle(attribIndex, attrib, *binding, issuedType)) {
                        continue;
                    }
                    // No shift here on purpose: a zero stride never advances. Binding index ==
                    // attribute index here, so this IS the attribute's divisor for the shadow.
                    g_GLESFuncs.glVertexBindingDivisor(attribIndex, divisor);
                    m_hostDivisorKnownMask |= 1u << attribIndex;
                    m_hostDivisor[attribIndex] = divisor;
                    continue;
                }

                if (MG_Pipe::MGPipeHandleIsNull(binding->Res)) {
                    MGLOG_W_ONCE("Attribute has no bound buffer, skipping.");
                    walkWasPlain = false;
                    continue;
                }
                const auto* attribBuffer = BufferImpl::FindBufferResourceForHandle(binding->Res);
                if (!attribBuffer || attribBuffer->id == 0) {
                    MGLOG_E_ONCE("No backend buffer found for attribute's buffer, cannot bind attribute.");
                    walkWasPlain = false;
                    continue;
                }

                const SizeT fetchOffset = static_cast<SizeT>(attrib.Offset) +
                                          BaseInstanceByteShiftWire(attrib.Stride, divisor, fetchBaseInstance);
                const GLenum glType =
                    IntegerAttribTypeForInput(MG_Util::ConvertDataTypeToGLEnum(static_cast<DataType>(attrib.Type)),
                                              attrib.IsInteger && ((retypeHeld >> attribIndex) & 1u));
                const GLint glSize = static_cast<GLint>(attrib.Size);

                // The pointer the driver VAO already holds for this attribute - the buffer it
                // captured included - needs no glBindBuffer + pointer call.
                auto& shadowPointer = m_hostPointer[attribIndex];
                const Bool pointerHeld =
                    VaoShadowOn() && shadowPointer.valid && shadowPointer.bufferId == attribBuffer->id &&
                    shadowPointer.size == glSize && shadowPointer.type == static_cast<Uint32>(glType) &&
                    shadowPointer.stride == static_cast<Int32>(attrib.Stride) && shadowPointer.offset == fetchOffset &&
                    shadowPointer.normalized == (attrib.Normalized != 0) &&
                    shadowPointer.integer == (attrib.IsInteger != 0);
                if (!pointerHeld) {
                    BufferImpl::BindBufferId(GL_ARRAY_BUFFER, attribBuffer->id);
                    if (!attrib.IsInteger) {
                        g_GLESFuncs.glVertexAttribPointer(attribIndex, glSize, glType,
                                                          attrib.Normalized ? GL_TRUE : GL_FALSE, attrib.Stride,
                                                          (const void*)fetchOffset);
                    } else {
                        g_GLESFuncs.glVertexAttribIPointer(attribIndex, glSize, glType, attrib.Stride,
                                                           (const void*)fetchOffset);
                    }
                    shadowPointer = {attribBuffer->id,
                                     glSize,
                                     static_cast<Uint32>(glType),
                                     static_cast<Int32>(attrib.Stride),
                                     fetchOffset,
                                     attrib.Normalized != 0,
                                     attrib.IsInteger != 0,
                                     true};
                }

                HostSetDivisor(attribIndex, divisor);
            }

            if (indexBufferDirty) {
                Bool indexBufferSynced = false;
                if (!MG_Pipe::MGPipeHandleIsNull(st.IndexBuffer.Res)) {
                    // RESOLVE ONLY, no ensure - unlike the legacy arm, which called
                    // EnsureBufferResource here. PrepareForDraw runs SyncNeccessaryBuffers before
                    // SyncCurrentVAO, so an INDEXED draw has already ensured this store; a
                    // non-indexed draw whose IndexBufferSerial moved has not, and then this logs
                    // once and leaves the binding alone. That is noisy rather than wrong: nothing
                    // is stamped on the miss (indexBufferSynced stays false), so the next indexed
                    // draw repairs it. Ensuring here would need the frontend object this arm
                    // deliberately does not hold.
                    auto* backendResource = BufferImpl::FindBufferResourceForHandle(st.IndexBuffer.Res);
                    if (backendResource && backendResource->id != 0) {
                        BufferImpl::BindBufferId(GL_ELEMENT_ARRAY_BUFFER, backendResource->id);
                        indexBufferSynced = true;
                    } else {
                        MGLOG_W_ONCE("No backend buffer found for index buffer binding, cannot bind index buffer.");
                    }
                } else {
                    g_GLESFuncs.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
                    indexBufferSynced = true;
                }
                if (indexBufferSynced) {
                    m_syncedIndexRes = st.IndexBuffer.Res;
                    m_syncedIndexValid = true;
                    // The two element-array restore scopes (the restart substitution's and
                    // MultiDrawImpl's) put the DRIVER id back without touching this serial,
                    // which is correct: the serial records what the APPLIER last said, and
                    // those scopes restored exactly what the applier said.
                    m_syncedIndexSerial = st.IndexBufferSerial;
                }
            }

            if (attributesDirty) {
                m_syncedElementsHandle = st.BoundVertexElements;
                m_syncedElementsSerial = rec->ContentSerial;
                m_syncedVertexBuffersSerial = st.VertexBuffersSerial;
                m_hasSyncedElements = true;
                // The window this walk emitted, for AdoptUnchangedBufferSets.
                m_syncedBufferStart = st.VertexBufferStart;
                m_syncedBufferCount =
                    std::min<Uint32>(st.VertexBufferCount, MG_Pipe::kMGPipeMaxVertexAttribs - st.VertexBufferStart);
                m_syncedBufferBaseInstance = st.VertexFetchBaseInstance;
                for (Uint32 i = 0; i < m_syncedBufferCount; ++i)
                    m_syncedBuffers[i] = st.VertexBuffers[m_syncedBufferStart + i];
                m_syncedBuffersValid = st.VertexBufferStart <= MG_Pipe::kMGPipeMaxVertexAttribs;
                m_syncedContextSerial = st.ContextSerial;
            }
            if (emitAttributes) {
                m_syncedFetchBaseInstance = fetchBaseInstance;
                m_syncedRetypeMask = retypeHeld;
                m_lastWalkWasPlain = walkWasPlain;
            }
            m_syncedBufferIdGeneration = currentBufferIdGeneration;
        }

        void BackendVertexArrayObject::AdoptUnchangedBufferSets(const MG_Pipe::MGPipeApplierState& st) {
            if (!m_hasSyncedElements || m_syncedContextSerial != st.ContextSerial) return;
            if (m_syncedBuffersValid && m_syncedVertexBuffersSerial != st.VertexBuffersSerial &&
                st.VertexBufferStart == m_syncedBufferStart &&
                st.VertexFetchBaseInstance == m_syncedBufferBaseInstance &&
                std::min<Uint32>(st.VertexBufferCount, MG_Pipe::kMGPipeMaxVertexAttribs - st.VertexBufferStart) ==
                    m_syncedBufferCount) {
                Bool same = true;
                for (Uint32 i = 0; same && i < m_syncedBufferCount; ++i) {
                    const auto& a = m_syncedBuffers[i];
                    const auto& b = st.VertexBuffers[m_syncedBufferStart + i];
                    same = a.Res == b.Res && a.Offset == b.Offset && a.Stride == b.Stride && a.Divisor == b.Divisor &&
                           a.BindingIndex == b.BindingIndex;
                }
                if (same) {
                    if (m_resolvedDrawBuffers.valid &&
                        m_resolvedDrawBuffers.buffersSerial == m_syncedVertexBuffersSerial)
                        m_resolvedDrawBuffers.buffersSerial = st.VertexBuffersSerial;
                    m_syncedVertexBuffersSerial = st.VertexBuffersSerial;
                }
            }
            if (m_syncedIndexValid && m_syncedIndexSerial != st.IndexBufferSerial &&
                m_syncedIndexRes == st.IndexBuffer.Res) {
                if (m_resolvedDrawBuffers.iboSerial == m_syncedIndexSerial)
                    m_resolvedDrawBuffers.iboSerial = st.IndexBufferSerial;
                m_syncedIndexSerial = st.IndexBufferSerial;
            }
        }

        void BackendVertexArrayObject::SyncClientSideAttributesForDrawArrays(
            const SharedPtr<MG_State::GLState::VertexArrayObject>& stateVAOObject, GLint first, GLsizei count,
            Uint32 fetchBaseInstance) {
            if (!stateVAOObject || count <= 0 || first < 0) {
                return;
            }
            // This rewrites the driver VAO's arrays behind SyncToBackendFromApplier's gate, so
            // the next sync must not take its buffers-only walk, nor adopt an equal buffer set.
            m_lastWalkWasPlain = false;
            m_syncedBuffersValid = false;
            HostForgetAll(); // the uploads below set enables, pointers and divisors directly

            Bind();

            const auto& allAttributes = stateVAOObject->GetAllAttributes();
            for (Uint attribIndex = 0; attribIndex < allAttributes.size(); ++attribIndex) {
                const auto& attrib = allAttributes[attribIndex];
                if (!attrib.Enabled || attrib.Buffer) {
                    continue;
                }

                // Same narrowing as SyncToBackend (the long note lives there), with the draw's own
                // fetch range standing in for the buffer extent a client array does not have. The
                // upload starts at element 0 so `first` keeps indexing the stream, exactly as the
                // unconverted upload below does. The test is on the storage rather than on IsLong
                // because glVertexAttribFormat(GL_DOUBLE) is 64-bit data without being long.
                if (attrib.IsLong || attrib.Type == DataType::Float64) {
                    const auto* sourceBase = reinterpret_cast<const Uint8*>(attrib.Offset);
                    if (attrib.Type != DataType::Float64 || sourceBase == nullptr || attrib.Size < 1 ||
                        attrib.Size > 4) {
                        g_GLESFuncs.glDisableVertexAttribArray(attribIndex);
                        continue;
                    }

                    auto& bufferId = m_clientAttributeBufferIds[attribIndex];
                    if (bufferId == 0) {
                        g_GLESFuncs.glGenBuffers(1, &bufferId);
                        if (bufferId == 0) {
                            MGLOG_E_ONCE("Failed to create client-side vertex attribute upload buffer.");
                            g_GLESFuncs.glDisableVertexAttribArray(attribIndex);
                            continue;
                        }
                    }

                    const SizeT componentCount = static_cast<SizeT>(attrib.Size);
                    const SizeT sourceElementSize = componentCount * sizeof(Double);
                    // A client array only ever reaches here through a pointer call, whose stride 0
                    // the frontend already resolved to the element size, so this is a guard rather
                    // than a case (VertexAttribute::Stride).
                    const SizeT sourceStride =
                        attrib.Stride > 0 ? static_cast<SizeT>(attrib.Stride) : sourceElementSize;
                    const SizeT elementCount = static_cast<SizeT>(first) + static_cast<SizeT>(count);
                    Vector<Float> converted;
                    NarrowDoubleStreamToFloat32(sourceBase, sourceStride, componentCount, elementCount, converted);

                    BufferImpl::BindBufferId(GL_ARRAY_BUFFER, bufferId);
                    g_GLESFuncs.glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(converted.size() * sizeof(Float)),
                                             converted.data(), GL_STREAM_DRAW);
                    if (MG_Util::PipeStats::Enabled()) {
                        MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::StageVertexClient,
                                                     static_cast<Uint64>(converted.size() * sizeof(Float)));
                    }
                    // GL ignores `normalized` for floating-point array types, so it is not
                    // forwarded here either.
                    g_GLESFuncs.glVertexAttribPointer(
                        attribIndex, attrib.Size, GL_FLOAT, GL_FALSE,
                        static_cast<GLsizei>(componentCount * sizeof(Float)),
                        // The converted stream is tightly packed, so a divisor'd array's
                        // baseInstance shift is a whole number of CONVERTED elements -
                        // not BaseInstanceByteShift's source-stride arithmetic.
                        reinterpret_cast<const void*>(attrib.Divisor == 0 ? 0u
                                                                          : static_cast<SizeT>(fetchBaseInstance) *
                                                                                componentCount * sizeof(Float)));
                    // glVertexAttribPointer's final argument IS that shift. A client array
                    // declared through the pointer API keeps element 0 at the buffer's byte 0, so
                    // the shift rides in the pointer argument the call above passes.
                    // The float64 narrowing itself is shared with SyncToBackend's conversion
                    // branch; only the fetch RANGE differs, and the draw's is the accurate one.
                    g_GLESFuncs.glEnableVertexAttribArray(attribIndex);
                    continue;
                }

                const auto* clientData = reinterpret_cast<const Uint8*>(attrib.Offset);
                const SizeT elementSize = GetAttributeByteSize(attrib.Type, attrib.Size, attrib.IsBgra);
                if (!clientData || elementSize == 0 || attrib.Size <= 0) {
                    continue;
                }

                const SizeT stride = attrib.Stride > 0 ? static_cast<SizeT>(attrib.Stride) : elementSize;
                const SizeT uploadSize = static_cast<SizeT>(first + count - 1) * stride + elementSize;

                auto& bufferId = m_clientAttributeBufferIds[attribIndex];
                if (bufferId == 0) {
                    g_GLESFuncs.glGenBuffers(1, &bufferId);
                    if (bufferId == 0) {
                        MGLOG_E_ONCE("Failed to create client-side vertex attribute upload buffer.");
                        continue;
                    }
                }

                // A GL_BGRA array is swizzled into RGBA order on the way, as on the buffer path
                // (SyncConvertedAttributeByHandle): ES has no BGRA vertex size to declare.
                const Bool bgra = attrib.IsBgra && !attrib.IsInteger;
                Vector<Uint8> swizzled;
                if (bgra) {
                    swizzled.assign(clientData, clientData + uploadSize);
                    for (SizeT element = 0; element < static_cast<SizeT>(first + count); ++element) {
                        SwizzleBgraElementToRgba(swizzled.data() + element * stride, attrib.Type);
                    }
                }

                BufferImpl::BindBufferId(GL_ARRAY_BUFFER, bufferId);
                g_GLESFuncs.glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(uploadSize),
                                         bgra ? static_cast<const void*>(swizzled.data()) : clientData, GL_STREAM_DRAW);
                if (MG_Util::PipeStats::Enabled()) {
                    MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::StageVertexClient,
                                                 static_cast<Uint64>(uploadSize));
                }

                if (!attrib.IsInteger) {
                    const GLint glSize = bgra ? 4 : attrib.Size;
                    g_GLESFuncs.glVertexAttribPointer(
                        attribIndex, glSize, MG_Util::ConvertDataTypeToGLEnum(attrib.Type),
                        attrib.Normalized ? GL_TRUE : GL_FALSE, static_cast<GLsizei>(stride),
                        // A client array's element 0 sits at its buffer's byte 0, so the
                        // divisor'd arrays' baseInstance shift is exactly BaseInstanceByteShift -
                        // the same arithmetic SyncToBackend applies to a VBO-backed array.
                        reinterpret_cast<const void*>(BaseInstanceByteShift(attrib, fetchBaseInstance)));
                } else {
                    g_GLESFuncs.glVertexAttribIPointer(
                        attribIndex, attrib.Size,
                        IntegerAttribTypeForInput(MG_Util::ConvertDataTypeToGLEnum(attrib.Type),
                                                  ((m_syncedRetypeMask >> attribIndex) & 1u) != 0),
                        static_cast<GLsizei>(stride),
                        // See the pointer call above: the divisor'd
                        // arrays' baseInstance shift is the pointer.
                        reinterpret_cast<const void*>(BaseInstanceByteShift(attrib, fetchBaseInstance)));
                }
            }
        }

        Bool BackendVertexArrayObject::SyncConvertedAttributeByHandle(Uint attribIndex,
                                                                      const MGPVertexAttribWire& attrib,
                                                                      const MG_Pipe::MGPVertexBuffer& binding,
                                                                      Uint32 fetchBaseInstance) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (attribIndex >= m_convertedAttributeBufferIds.size() || attrib.Size < 1 || attrib.Size > 4) {
                return false;
            }
            // Which of the two conversions: a BGRA array is a 4-byte element whatever its type,
            // and only the three types GL allows with it reach here (the frontend refuses the
            // rest at the format call).
            const DataType sourceType = static_cast<DataType>(attrib.Type);
            const Bool bgra = attrib.IsBgra != 0;
            if (bgra && sourceType != DataType::Uint8 && sourceType != DataType::Int2101010Rev &&
                sourceType != DataType::Uint2101010Rev) {
                return false;
            }
            if (!bgra && sourceType != DataType::Float64) return false;
            if (MG_Pipe::MGPipeHandleIsNull(binding.Res)) {
                // A client-memory 64-bit array is narrowed on the draw path instead, which is
                // the only place its fetch range is known.
                return false;
            }
            auto* resource = BufferImpl::FindBufferResourceForHandle(binding.Res);
            if (resource == nullptr) return false;

            // WHAT IS NOT HERE, AND IT IS A RULED DEVIATION FOR P3a RATHER THAN AN OVERSIGHT
            // (M-3; integrator ruling, this commit).
            //
            // The legacy arm opens with bufferObject->SyncGpuWrites(), one of the eleven Espryt
            // SyncPersistentMappedRange / SyncGpuWrites sites D-N keeps where they are for P3a.
            // D-N's wording is "no MOVE of those sites off the frontend", and this arm does not
            // move it: it CANNOT MAKE IT AT ALL. The call needs a frontend BufferObject and this
            // path holds only a handle, because the server has no inverse map back to a frontend
            // object - that absence is the design, not a gap in it (ARCHITECTURE.md 4.2). The
            // two ways to keep the site here would each break something D-N or D-J protects: a
            // handle -> object map is the very thing the split removes, and pulling the bytes
            // eagerly on the client at every draw is new behaviour and new cost.
            //
            // SO THE DEVIATION IS DECLARED, WITH ITS BLAST RADIUS. Under the default mask a
            // 64-bit vertex array whose SOURCE buffer was written by a shader and not yet pulled
            // back narrows STALE bytes on this arm and fresh bytes on the legacy one. That is
            // the whole of it: fp64 vertex arrays, fed by a buffer a shader wrote, read without
            // any intervening explicit readback. No other attribute type reads through this
            // path, and a persistently mapped source is excluded separately below (the memo
            // never trusts one, so those re-read every draw through the coherent map).
            //
            // P8-C CLOSES IT UNDER A TRANSPORT, on the server rather than by moving the pull to the
            // client: a source a shader wrote on this side is read back from this server's own GL
            // buffer (SplitHostBytesForCpuRead), and that write also voids the memo below, whose
            // serial a GPU write never moves. The MONOLITH arm of this function keeps the
            // deviation (its hostBytes is the client's shadow and its mark is the frontend
            // object's); that half is dev's (ID-P8-3).
            // WHERE THE BYTES ARE, and it is not one fixed place: an ADOPTED store has no client
            // shadow left at all (PipeResource::AdoptPersistentMap clears and shrinks it), so
            // the coherent map IS the source of truth - which is exactly the case the memo
            // exclusion below makes the most frequent, since a persistently mapped source is
            // never trusted and this narrowing therefore re-reads on every draw. Reading the
            // recorded shadow base for such a resource read freed memory; hostBytes is nulled
            // at adoption for the same reason, so the fallback below is a null and a refusal
            // rather than a stale read.
            const Bool sourceGpuWritten = resource->serverGpuWritten;
            if (sourceGpuWritten) {
                BufferImpl::SplitHostBytesForCpuRead(*resource, binding.Res,
                                                     bgra ? "bgra_vertex_swizzle" : "fp64_vertex_narrowing");
            }
            const Uint8* const sourceBase = (resource->persistentMapped && resource->persistentPtr != nullptr)
                                                ? static_cast<const Uint8*>(resource->persistentPtr)
                                                : resource->hostBytes;
            const SizeT sourceSize = BufferImpl::ResourceWidthForHandle(binding.Res);
            if (sourceBase == nullptr || attrib.Offset >= sourceSize) {
                return false;
            }

            // A BGRA element is always one 32-bit word, and so is its RGBA replacement; a 64-bit
            // element narrows to the float32 vector of the same component count.
            const SizeT componentCount = static_cast<SizeT>(attrib.Size);
            const SizeT sourceElementSize = bgra ? 4 : componentCount * sizeof(Double);
            const SizeT convertedElementSize = bgra ? 4 : componentCount * sizeof(Float);
            const SizeT available = sourceSize - static_cast<SizeT>(attrib.Offset);
            if (available < sourceElementSize) {
                return false;
            }

            const Bool neverAdvances = attrib.Stride <= 0;
            const SizeT sourceStride = neverAdvances ? sourceElementSize : static_cast<SizeT>(attrib.Stride);
            const SizeT elementCount = neverAdvances ? 1 : ((available - sourceElementSize) / sourceStride) + 1;

            const SizeT firstElement = (fetchBaseInstance != 0 && binding.Divisor != 0 && !neverAdvances)
                                           ? static_cast<SizeT>(fetchBaseInstance)
                                           : 0;
            if (firstElement >= elementCount) {
                return false;
            }

            auto& stream = m_convertedAttributeStreams[attribIndex];
            Uint& convertedBufferId = m_convertedAttributeBufferIds[attribIndex];
            const Uint64 sourceSerial = BufferImpl::ResourceSerialForHandle(binding.Res);
            // The memo, re-keyed: the pin is the source buffer's {slot, gen} instead of a
            // frontend lifetime id, and the freshness input is the applier's server-owned
            // Serial instead of the frontend change serial. A PERSISTENTLY MAPPED source is
            // still never trusted - it is written through the pointer with no call at all, so
            // no serial on either side can prove the converted copy is current. The conversion
            // itself is part of the key: one attribute index can carry both shapes over its life,
            // over the same bytes.
            const Bool memoHit =
                stream.valid && convertedBufferId != 0 && !resource->persistentMapped && !sourceGpuWritten &&
                stream.sourceHandle == binding.Res && stream.sourceChangeSerial == sourceSerial &&
                stream.sourceOffset == static_cast<SizeT>(attrib.Offset) && stream.sourceStride == sourceStride &&
                stream.bgra == bgra && stream.sourceType == attrib.Type && stream.componentCount == componentCount &&
                stream.elementCount == elementCount;
            if (!memoHit) {
                if (convertedBufferId == 0) {
                    g_GLESFuncs.glGenBuffers(1, &convertedBufferId);
                    if (convertedBufferId == 0) {
                        MGLOG_E_ONCE("Failed to create the scratch buffer for the converted vertex array at "
                                     "attribute %u.",
                                     attribIndex);
                        return false;
                    }
                }
                Vector<Float> narrowed;
                Vector<Uint8> swizzled;
                const void* convertedData = nullptr;
                SizeT convertedBytes = 0;
                if (bgra) {
                    SwizzleBgraStreamToRgba(sourceBase + attrib.Offset, sourceStride, elementCount, sourceType,
                                            swizzled);
                    convertedData = swizzled.data();
                    convertedBytes = swizzled.size();
                } else {
                    NarrowDoubleStreamToFloat32(sourceBase + attrib.Offset, sourceStride, componentCount, elementCount,
                                                narrowed);
                    convertedData = narrowed.data();
                    convertedBytes = narrowed.size() * sizeof(Float);
                }
                BufferImpl::BindBufferId(GL_ARRAY_BUFFER, convertedBufferId);
                g_GLESFuncs.glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(convertedBytes), convertedData,
                                         GL_STREAM_DRAW);
                if (MG_Util::PipeStats::Enabled()) {
                    MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::StageVertexClient,
                                                 static_cast<Uint64>(convertedBytes));
                }
                stream.valid = true;
                stream.sourceHandle = binding.Res;
                stream.sourceChangeSerial = sourceSerial;
                stream.sourceOffset = static_cast<SizeT>(attrib.Offset);
                stream.sourceStride = sourceStride;
                stream.bgra = bgra;
                stream.sourceType = attrib.Type;
                stream.componentCount = componentCount;
                stream.elementCount = elementCount;
                MGLOG_D("DirectGLES: converted the %s vertex array at attribute %u, %zu element(s).",
                        bgra ? "BGRA" : "64-bit", attribIndex, elementCount);
            }

            // What the converted stream is to the driver. The swizzled BGRA stream keeps its
            // source type and `normalized` (GL accepts BGRA only normalized, and a packed type's
            // signedness is the type's own). The narrowed stream is float32, and `normalized` is
            // deliberately GL_FALSE there rather than attrib.Normalized: GL ignores it for
            // floating-point array types, and honouring it would scale the fetched values.
            const GLint convertedSize = bgra ? 4 : static_cast<GLint>(attrib.Size);
            const GLenum convertedType = bgra ? MG_Util::ConvertDataTypeToGLEnum(sourceType) : GL_FLOAT;
            const GLboolean convertedNormalized = (bgra && attrib.Normalized) ? GL_TRUE : GL_FALSE;
            if (neverAdvances) {
                // Only the binding-point API can say "stride 0".
                if (!HasVertexBindingApi()) {
                    return false;
                }
                g_GLESFuncs.glVertexAttribFormat(attribIndex, convertedSize, convertedType, convertedNormalized, 0);
                g_GLESFuncs.glVertexAttribBinding(attribIndex, attribIndex);
                g_GLESFuncs.glBindVertexBuffer(attribIndex, convertedBufferId, 0, 0);
                return true;
            }

            BufferImpl::BindBufferId(GL_ARRAY_BUFFER, convertedBufferId);
            g_GLESFuncs.glVertexAttribPointer(attribIndex, convertedSize, convertedType, convertedNormalized,
                                              static_cast<GLsizei>(convertedElementSize),
                                              (const void*)(firstElement * convertedElementSize));
            return true;
        }

        TwinRegistry<MG_State::GLState::VertexArrayObject, BackendVertexArrayObject,
                     MG_Pipe::MGPipeKind::VertexElementsCso>
            g_backendVertexArrayObjects;
    } // namespace VertexArrayImpl
} // namespace MobileGL::MG_Backend::DirectGLES
