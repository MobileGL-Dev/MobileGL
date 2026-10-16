// MobileGL - MobileGL/MG_Backend/DirectGLES/DirectGLESBuffers.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "DirectGLESInternal.h"

namespace MobileGL::MG_Backend::DirectGLES {
    namespace BufferImpl {
        // See Managers.h for why these two are named functions over the applier state and
        // nothing else.
        Uint ResolveDrawVertexBuffersFromRecord(const MG_Pipe::MGPipeApplierState& st, DrawVertexBufferRequest* out,
                                                Uint capacity) {
            Uint count = 0;
            const Uint32 begin = st.VertexBufferStart;
            const Uint32 end = begin + st.VertexBufferCount;
            for (Uint32 i = begin; i < end && i < MG_Pipe::kMGPipeMaxVertexAttribs; ++i) {
                const MG_Pipe::MGPVertexBuffer& binding = st.VertexBuffers[i];
                if (MG_Pipe::MGPipeHandleIsNull(binding.Res)) continue;

                // A {slot, gen} compare, not a raw address compare: an address a successor
                // object can reproduce is the identity hazard this phase exists to remove.
                Bool alreadySeen = false;
                for (Uint j = 0; j < count; ++j) {
                    if (out[j].Res == binding.Res) {
                        alreadySeen = true;
                        break;
                    }
                }
                if (alreadySeen) continue;
                if (count >= capacity) {
                    MGLOG_E_ONCE("MGPipe: the vertex-buffer window named more distinct buffers than "
                                 "a draw can hold (%u); the tail is dropped",
                                 capacity);
                    break;
                }
                out[count].Res = binding.Res;
                out[count].BindingIndex = binding.BindingIndex;
                ++count;
            }
            return count;
        }

        DrawIndexBufferRequest ResolveDrawIndexBufferFromRecord(const MG_Pipe::MGPipeApplierState& st) {
            DrawIndexBufferRequest request;
            request.Res = st.IndexBuffer.Res;
            request.Serial = st.IndexBufferSerial;
            return request;
        }

        // True from a draw or dispatch that ran with a storage buffer bound until the next
        // barrier that orders storage-buffer writes. Every bound storage point counts as written,
        // the client's own conservative set (see serverGpuWritten below).
        //
        // WHAT IT IS FOR: two dispatches with no barrier between them that write DIFFERENT
        // components of one vector in the same storage buffer. GL leaves the two unordered, which
        // allows them to run concurrently, but every write still lands - they are separate memory
        // locations. A driver that stores a vector component as a read-modify-write of the whole
        // vector loses one of them instead: KHR-GL43.shader_storage_buffer_object.
        // advanced-switchBuffers-cs writes digest.x .. digest.w of one uvec4 from four
        // back-to-back dispatches and read back a zero component in 8 runs of 20 on Adreno (0 of
        // 20 with a storage barrier between the dispatches, 0 of 20 with uint[4]). The fix is the
        // barrier the application did not owe: PrepareForCompute issues it when the previous
        // shader work may have written a storage buffer and nothing ordered those writes since.
        //
        // Compute only. A dispatch that follows a dispatch is the shape the test has and the one
        // a driver may overlap; draws keep their own ordering and are not taxed, they only SET
        // the flag here - one store in the walk that already visits every bound point.
        Bool g_shaderStorageWritesUnordered = false;

        // The record arm of SyncBufferBindingPoints. Same shape, same order, same branches; the
        // four frontend reads become the applier's window and a handle per entry.
        //
        // WHAT THE WINDOW IS: ShaderBufferCount[class] starting at ShaderBufferStart[class],
        // which is the client's touched high-water mark clamped to the 84 the wire carries
        // (ruling 10). It is NOT clamped on the client to the device's ceiling, so the ES clamp
        // below stays exactly where it was - a client reading GL_MAX_UNIFORM_BUFFER_BINDINGS
        // would be answering a driver question from the wrong side of the wire.
        //
        // WHOLE-VS-RANGE IS `Offset == 0 && Size == kMGPipeWholeBuffer` and obj->GetSize()
        // disappears from this function: a base binding does not freeze an extent, the client
        // deliberately does not resolve one, and the re-resolution happens HERE against the
        // server's own storage - which is what makes a glBufferData issued between the emission
        // and this apply bind the NEW extent, as GL does.
        void SyncBufferBindingPointsByRecord(Uint32 shaderBufferClass, GLenum glTarget) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            const MG_Pipe::MGPipeApplierState& st = MG_Pipe::MGPipeApplier();
            const Uint32 start = st.ShaderBufferStart[shaderBufferClass];
            SizeT end = static_cast<SizeT>(start) + st.ShaderBufferCount[shaderBufferClass];
            // ...and never past what the ES driver itself can hold, exactly as the frontend
            // walk does and for the same reason recorded there.
            if (glTarget == GL_UNIFORM_BUFFER && g_GLESCapabilities.MaxUniformBufferBindings > 0) {
                end = std::min(end, static_cast<SizeT>(g_GLESCapabilities.MaxUniformBufferBindings));
            }
            for (SizeT i = start; i < end; ++i) {
                const MG_Pipe::MGPBufferRange& entry = st.BoundShaderBuffers[shaderBufferClass][i];
                if (MG_Pipe::MGPipeHandleIsNull(entry.Res)) {
                    BindBufferBaseCached(glTarget, static_cast<GLuint>(i), 0);
                    continue;
                }
                auto* backendResource = EnsureBufferResourceForHandle(nullptr, entry.Res);
                if (!backendResource || backendResource->id == 0) {
                    MGLOG_E_ONCE("No backend buffer found for %s binding point %zu (handle {%u, %u}).",
                                 MG_Util::ConvertGLEnumToString(glTarget).c_str(), i, entry.Res.Slot, entry.Res.Gen);
                    continue;
                }
                // P8-C: the SERVER's half of the mark MarkShaderStorageBuffersGpuWritten no longer
                // makes under a transport - on this side's twin, for this side's CPU readers
                // (SplitHostBytesForCpuRead). Every bound storage point, which is the client's
                // own conservative set (ShaderBufferEmit.h marks every one writable); a uniform
                // point is read-only by definition.
                if (shaderBufferClass != MG_Pipe::kMGPipeShaderBufferClassUniform) {
                    backendResource->serverGpuWritten = true;
                    g_shaderStorageWritesUnordered = true;
                }
                const auto backendBufferId = backendResource->id;
                if (entry.Offset == 0 && entry.Size == MG_Pipe::kMGPipeWholeBuffer) {
                    BindBufferBaseCached(glTarget, static_cast<GLuint>(i), backendBufferId);
                } else {
                    // CLAMPED AGAINST THE SERVER's OWN STORAGE, which is what the frontend walk
                    // clamped against obj->GetSize() for: a range that outruns the storage is a
                    // GL_INVALID_VALUE the driver would raise on a bind the application already
                    // made legally against a buffer that has since shrunk.
                    const SizeT storage = backendResource->storageSize;
                    const SizeT rangeStart = std::min<SizeT>(static_cast<SizeT>(entry.Offset), storage);
                    const SizeT rangeEnd = std::min<SizeT>(static_cast<SizeT>(entry.Offset + entry.Size), storage);
                    BindBufferRangeCached(glTarget, static_cast<GLuint>(i), backendBufferId,
                                          static_cast<GLintptr>(rangeStart),
                                          static_cast<GLsizeiptr>(rangeEnd - rangeStart));
                }
            }
        }

        void SyncAtomicCounterBuffers(const Vector<Int>& glBindings, Int esslBindingTop) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            // P5e (sb, §5.6): THE RECORD ARM. `glBindings` stays the backend program's own list
            // - it is server-owned since the link and names which GL counter bindings THIS
            // program declares - and only the points themselves move sides. The bound is the
            // applier's window instead of GetBufferBindingPointCount's sticky forward, which is
            // the row this site was the Espryt reader of.
            const MG_Pipe::MGPipeApplierState& st = MG_Pipe::MGPipeApplier();
            const Uint32 windowStart = st.ShaderBufferStart[MG_Pipe::kMGPipeShaderBufferClassAtomicCounter];
            const SizeT windowEnd =
                static_cast<SizeT>(windowStart) + st.ShaderBufferCount[MG_Pipe::kMGPipeShaderBufferClassAtomicCounter];
            for (const Int glBinding : glBindings) {
                if (glBinding < 0) continue;
                const Int esslBinding = esslBindingTop - glBinding;
                // Already diagnosed once when the block was transpiled; nothing was bound to
                // it there either, so there is nothing to unbind here.
                if (esslBinding < 0) continue;
                // OUTSIDE THE WINDOW IS "NOTHING BOUND", which is what the frontend array's
                // default says too - so it takes the unbind arm rather than being skipped.
                // Skipping would leave whatever the driver last had at that ESSL point,
                // which is the hole the frontend walk's `continue` never had because its
                // bound was the whole 84-point array.
                const MG_Pipe::MGPBufferRange* entry = nullptr;
                if (static_cast<SizeT>(glBinding) >= windowStart && static_cast<SizeT>(glBinding) < windowEnd) {
                    entry = &st.BoundShaderBuffers[MG_Pipe::kMGPipeShaderBufferClassAtomicCounter]
                                                  [static_cast<SizeT>(glBinding)];
                }
                if (entry == nullptr || MG_Pipe::MGPipeHandleIsNull(entry->Res)) {
                    BindBufferBaseCached(GL_SHADER_STORAGE_BUFFER, static_cast<Uint>(esslBinding), 0);
                    continue;
                }
                auto* backendResource = EnsureBufferResourceForHandle(nullptr, entry->Res);
                if (!backendResource || backendResource->id == 0) {
                    MGLOG_E_ONCE("No backend buffer found for atomic counter binding point %d "
                                 "(handle {%u, %u}).",
                                 glBinding, entry->Res.Slot, entry->Res.Gen);
                    continue;
                }
                if (entry->Offset == 0 && entry->Size == MG_Pipe::kMGPipeWholeBuffer) {
                    BindBufferBaseCached(GL_SHADER_STORAGE_BUFFER, static_cast<Uint>(esslBinding), backendResource->id);
                } else {
                    const SizeT storage = backendResource->storageSize;
                    const SizeT rangeStart = std::min<SizeT>(static_cast<SizeT>(entry->Offset), storage);
                    const SizeT rangeEnd = std::min<SizeT>(static_cast<SizeT>(entry->Offset + entry->Size), storage);
                    BindBufferRangeCached(GL_SHADER_STORAGE_BUFFER, static_cast<Uint>(esslBinding), backendResource->id,
                                          static_cast<GLintptr>(rangeStart),
                                          static_cast<GLsizeiptr>(rangeEnd - rangeStart));
                }
                // AND NO MarkBufferGpuWritten HERE. It was the second of the three backend
                // GPU-write sites and it reached into a client object; the client's
                // MarkAtomicCounterBindings (GpuWritePending.cpp) already marks the same set
                // from MarkGpuWritesForDraw, WIDER on purpose - it marks every touched
                // counter point rather than only the ones this program declares, which is
                // the safe direction for an over-approximate set.
                //
                // P8-C: the server's own twin is marked instead, for the server's own CPU
                // readers (SplitHostBytesForCpuRead) - a counter buffer later bound as a
                // parameter or element buffer is read on this side.
                backendResource->serverGpuWritten = true;
            }
            return;
        }
    } // namespace BufferImpl

    namespace XfbImpl {
        // One scratch ES buffer serves every scattered capture: only one span can be
        // recording at a time (the driver would reject a second Begin), so its contents
        // are consumed by the End that follows.
        GLuint g_scatterBufferId = 0;
        SizeT g_scatterBufferSize = 0;

        UnorderedMap<GLuint, XfbObjectState> g_xfbObjects;
        GLuint g_currentXfbName = 0;
        // Cached address of g_xfbObjects[g_currentXfbName]: PrepareForDraw consults
        // CurrentXfb on EVERY draw (StartPendingTransformFeedback) and the map
        // lookup was pure per-draw overhead for the overwhelmingly common no-capture
        // case. Open addressing keeps values in the bucket array, so ANY insert can
        // rehash and move them - and erase moves them too, by shifting the rest of the
        // probe cluster into the hole, which reaches entries other than the erased one.
        // Every site that mutates the map or rebinds the current name resets this to
        // null instead of reasoning about stability, and CurrentXfb re-resolves lazily.
        XfbObjectState* g_currentXfbState = nullptr;
        // P13 W6: one role - the record arm's (the frontend arm's GL-name keyed state went).
        Array<RoleXfbState, 1> g_roleXfbState;

        namespace {
            RoleXfbState& ActiveXfbState() {
                auto& state = g_roleXfbState[0];
                if (state.NativeGeneration != g_backendContextGeneration) {
                    state = {};
                    state.NativeGeneration = g_backendContextGeneration;
                }
                return state;
            }
        } // namespace

        XfbObjectState& CurrentXfb() {
            auto& state = ActiveXfbState();
            const Uint64 lifetime = MG_Pipe::MGPipeApplier().BoundStreamOutputLifetimeId;
            if (state.Current != lifetime || state.Cached == nullptr || state.NeedsBind)
                BindTransformFeedback(0); // identity is the applier's lifetime, not this argument
            auto& g_currentXfbState = state.Cached;
            auto& g_xfbObjects = state.Objects;
            const auto g_currentXfbName = state.Current;
            if (g_currentXfbState == nullptr) {
                g_currentXfbState = &g_xfbObjects[g_currentXfbName];
            }
            return *g_currentXfbState;
        }

        namespace {
            Bool AreTransformFeedbackObjectsSupported() {
                return g_GLESFuncs.glGenTransformFeedbacks != nullptr &&
                       g_GLESFuncs.glBindTransformFeedback != nullptr &&
                       g_GLESFuncs.glDeleteTransformFeedbacks != nullptr &&
                       g_GLESFuncs.glPauseTransformFeedback != nullptr &&
                       g_GLESFuncs.glResumeTransformFeedback != nullptr;
            }

            // How wide one OnBufferWriteback may be for a capture range of `rangeBytes`, which is
            // the whole range when the transport imposes no limit (monolith, where the callback is
            // a direct call with no ring under it) or when the range already fits one.
            //
            // BOTH XFB WRITEBACK PRODUCERS ASK THE SAME FUNCTION. They post from different sources
            // - one from a live glMapBufferRange mapping, one from a staged Vector<Uint8> - but
            // they post the same KIND of record into the same ring, and a second spelling of the
            // width is how one of them ends up being the producer that still kills the server.
            SizeT XfbWritebackSliceStep(SizeT rangeBytes) {
                const Uint64 slice = MG_Pipe::MGPipeBufferWritebackSliceBytes();
                if (slice == 0 || slice >= static_cast<Uint64>(rangeBytes)) return rangeBytes;
                return static_cast<SizeT>(slice);
            }

            // Mirrors one capture span's results into the frontend CPU shadows. The GPU wrote
            // the capture buffers behind the frontend's back, so the shadows that back
            // MapBuffer/GetBufferSubData still hold the pre-draw bytes. Buffers whose storage
            // the backend already owns (coherent persistent map) need nothing: reads resolve
            // against that storage directly.
            void ReadbackCapturedRanges(Vector<XfbCaptureTarget>& targets) {
                if (g_GLESFuncs.glMapBufferRange == nullptr || g_GLESFuncs.glUnmapBuffer == nullptr) {
                    MGLOG_E_ONCE("EndTransformFeedback: the ES driver exposes no glMapBufferRange/glUnmapBuffer, so "
                                 "captured data can never reach the application's buffers");
                }
                if (targets.empty()) {
                    // The span closed with nothing to mirror back. Either the deferred Begin
                    // never ran (a span with no draw - legal) or it ran and found no bound
                    // capture buffer, which is not.
                    MGLOG_D("EndTransformFeedback: capture span closed with no recorded targets");
                }
                if (g_GLESFuncs.glMapBufferRange != nullptr && g_GLESFuncs.glUnmapBuffer != nullptr) {
                    for (const auto& target : targets) {
                        // P5c: under an active transport the frontend object is client
                        // memory (rule E), so the persistence question is the server
                        // resource's and the captured bytes go back as a writeback EVENT -
                        // WritebackFromBackend from the apply thread is the R1/R2 shape.
                        const MG_Pipe::MGPipeHandle res = target.handle;
                        auto* resource = BufferImpl::FindBufferResourceForHandle(res);
                        if (resource == nullptr) continue;
                        if (resource->persistentMapped) {
                            // P8-C: nothing to copy - the capture reaches a coherent map -
                            // but this side's CPU readers must finish before reading it.
                            resource->serverGpuWritten = true;
                            continue;
                        }
                        const SizeT size = target.end - target.start;
                        BufferImpl::BindBufferId(BufferImpl::TempBufferTarget, target.backendId);
                        void* mapped = g_GLESFuncs.glMapBufferRange(BufferImpl::TempBufferTarget,
                                                                    static_cast<GLintptr>(target.start),
                                                                    static_cast<GLsizeiptr>(size), GL_MAP_READ_BIT);
                        if (mapped == nullptr) {
                            MGLOG_E_ONCE("EndTransformFeedback: failed to map backend buffer %u [%zu, %zu) for "
                                         "capture readback (ES error %s); the captured data will NOT be visible to "
                                         "the application",
                                         target.backendId, target.start, target.end,
                                         MG_Util::ConvertGLEnumToString(TakeXfbDriverError()).c_str());
                            continue;
                        }
                        if (MG_Pipe::gMGPipeCallbacks.OnBufferWriteback != nullptr) {
                            // ONE EVENT PER SLICE, NOT ONE PER CAPTURE. The bytes travel
                            // INLINE in a SEG_EVENT record and the ring refuses any record
                            // above half its capacity outright, so a whole-range post of a
                            // capture wider than ~128 KiB was Fatal{EventRingOverflow} - the
                            // SERVER dying on a large buffer. The slicing shape is
                            // BufferObject.cpp's one ring over: in-order delivery makes the
                            // last slice's landing imply every earlier one, so the client
                            // needs nothing new to reassemble them.
                            //
                            // INSIDE THE MAPPING, deliberately: the map is the expensive part
                            // and the slices are reads of one live pointer, so this unmaps
                            // once below however many events it posts.
                            const SizeT step = XfbWritebackSliceStep(size);
                            for (SizeT off = 0; off < size; off += step) {
                                const SizeT bytes = std::min(step, size - off);
                                MG_Pipe::gMGPipeCallbacks.OnBufferWriteback(
                                    res, target.start + off,
                                    MG_Pipe::MGPBlobRef{reinterpret_cast<Uint64>(static_cast<Uint8*>(mapped) + off),
                                                        static_cast<Uint64>(bytes), MG_Pipe::kMGHostSpanSegNone, 0});
                            }
                        } else {
                            MGLOG_E_ONCE("EndTransformFeedback: no reverse channel is installed, so the "
                                         "captured bytes of buffer {%u,%u} cannot reach the client shadow",
                                         res.Slot, res.Gen);
                        }
                        // P8-C: the server's own staged copy takes the same bytes, from the
                        // same mapping, so this side's CPU readers see the capture too.
                        BufferImpl::StageServerWrittenRange(*resource, res, mapped, target.start, size);
                        g_GLESFuncs.glUnmapBuffer(BufferImpl::TempBufferTarget);
                        // Ops_H_Readback's order: writeback, unmap, THEN the serial
                        // stamp, so the next draw does not re-upload the server's stale
                        // staged bytes over the capture that just landed.
                        resource->syncedChangeSerial = BufferImpl::ResourceSerialForHandle(res);
                        BufferImpl::BumpBufferMutationEpoch();
                        continue;
                    }
                }
                targets.clear();
            }

            // Distributes the gap-free records the driver captured into the application's
            // buffers at the offsets the GL layout asks for. Only the bytes a varying actually
            // occupies are written, so the holes gl_SkipComponents asks for keep whatever the
            // application had put there - which is the whole point of the feature.
            void ScatterCapturedRecords(XfbObjectState& xfb) {
                auto& g_scatterBufferId = xfb.serverScatterBuffer;
                const XfbProgramSource program{xfb.scatterProgram, xfb.scatterArchive};
                if (!program || xfb.targets.empty()) return;
                if (g_GLESFuncs.glMapBufferRange == nullptr || g_GLESFuncs.glUnmapBuffer == nullptr) return;

                const SizeT packedStride = program->GetTransformFeedbackPackedStride();
                const SizeT modelledVertices =
                    static_cast<SizeT>(MG_Pipe::gPipeInputs->GetTransformFeedbackCapturedVertices());
                const SizeT vertices = std::min<SizeT>(modelledVertices, xfb.scatterCapacityVertices);
                if (packedStride == 0 || vertices == 0) {
                    // The scatter path redirected the DRIVER's capture into the scratch buffer,
                    // so bailing here leaves the application's buffers holding their pre-draw
                    // bytes - a total data loss, not a no-op. The vertex count is the CPU model
                    // (AccountTransformFeedbackPrimitives), which is 0 for any draw mode
                    // CountPrimitivesForDraw does not know and for the instanced/indirect entry
                    // points that never call it.
                    MGLOG_E_ONCE("EndTransformFeedback: scattered capture discarded - packedStride=%zu, "
                                 "CPU-modelled captured vertices=%zu, scratch capacity=%zu. The capture buffers keep "
                                 "their pre-draw contents.",
                                 packedStride, modelledVertices, xfb.scatterCapacityVertices);
                    return;
                }

                BufferImpl::BindBufferId(BufferImpl::TempBufferTarget, g_scatterBufferId);
                const void* packed = g_GLESFuncs.glMapBufferRange(
                    BufferImpl::TempBufferTarget, 0, static_cast<GLsizeiptr>(packedStride * vertices), GL_MAP_READ_BIT);
                if (packed == nullptr) {
                    MGLOG_E_ONCE("EndTransformFeedback: failed to map the scatter capture buffer");
                    return;
                }

                // One staged copy per destination buffer: start from what the application had
                // (the shadow is authoritative - uploads go shadow -> ES, and previous captures
                // were mirrored back into it), patch the captured varyings in, then push the
                // whole range down once.
                for (SizeT targetIndex = 0; targetIndex < xfb.targets.size(); ++targetIndex) {
                    const auto& target = xfb.targets[targetIndex];
                    // By BUFFER index, not by position in the compacted list - see XfbCaptureTarget.
                    const SizeT stride = program->GetTransformFeedbackStride(static_cast<Uint32>(target.bufferIndex));
                    if (stride == 0) continue;
                    const SizeT rangeBytes = target.end - target.start;
                    Vector<Uint8> staged(rangeBytes);
                    // P5c: under an active transport BufferObject::MappedData on the apply
                    // thread is Fatal{RoleViolation, "buffer-legacy-arm"} (CONTRACT-P5C §3.8) -
                    // the pre-capture bytes are the SERVER's staged shadow, and the reconciled
                    // range goes back as a writeback EVENT instead of a WritebackFromBackend
                    // poke into client memory.
                    MG_Pipe::MGPipeHandle splitRes = MG_Pipe::kMGPipeNullHandle;
                    BufferImpl::GLESBufferResource* splitResource = nullptr;
                    splitRes = target.handle;
                    splitResource = BufferImpl::FindBufferResourceForHandle(splitRes);
                    if (splitResource == nullptr) {
                        // THE ONLY DISCARD LEFT, and it is the impossible one: this target
                        // was recorded by BindBufferRange/Base against a handle the applier
                        // had already minted a twin for, so no twin here means the slot
                        // table and the capture list disagree about one resource. There is
                        // no backendId to upload into either, so there is nothing to do but
                        // name it.
                        MGLOG_E_ONCE("EndTransformFeedback: a scattered capture target (handle {%u,%u}) has "
                                     "no server twin at all; its capture is discarded",
                                     splitRes.Slot, splitRes.Gen);
                        continue;
                    }
                    // P8-C: made current first - a store a shader wrote here is refreshed.
                    const Uint8* hostBytes =
                        BufferImpl::SplitHostBytesForCpuRead(*splitResource, splitRes, "xfb_scatter_pre_capture");
                    // A NULL SHADOW IS NOT A LOST CAPTURE. It is the ORPHANED store
                    // (glBufferData(size, NULL) with no resource_subdata since), which
                    // M-3 declares legal: every byte the application has not staged is
                    // UNDEFINED by its own declaration, and `staged` is already
                    // value-initialised to zero - which is byte for byte what the monolith
                    // arm scatters over when it reads a fresh MappedData(). Skipping the
                    // Memcpy is therefore the WHOLE of the difference; skipping the TARGET
                    // threw the varyings away and left the application's buffer holding its
                    // pre-draw bytes under GL_NO_ERROR, the silent-loss class this file's
                    // preamble exists to close. The streaming idiom that hits it - orphan,
                    // partial glBufferSubData, capture into the rest - is ordinary.
                    if (hostBytes != nullptr) {
                        // Managers.cpp's pool-reuse ladder guards its whole-store
                        // MGL_SERVER_STAGED_REQUIRE with exactly this predicate and for
                        // exactly this reason: for a store whose content the application
                        // SUPPLIED a coverage gap is a missing record and zero-filling past
                        // it is silent data loss (Fatal by name); for an orphaned one the
                        // gap is its own undefined content and the read is legal.
                        if (BufferImpl::ResourceContentIsDeclared(splitRes)) {
                            BufferImpl::RequireStagedCoverage(*splitResource, hostBytes, target.start, target.end,
                                                              "xfb_scatter_pre_capture");
                        }
                        Memcpy(staged.data(), hostBytes + target.start, rangeBytes);
                    }

                    for (const auto& varying : program->GetTransformFeedbackVaryings()) {
                        if (varying.bufferIndex != target.bufferIndex) continue;
                        for (SizeT v = 0; v < vertices; ++v) {
                            const SizeT dstOffset = v * stride + varying.offsetBytes;
                            if (dstOffset + varying.byteSize > rangeBytes) break;
                            Memcpy(staged.data() + dstOffset,
                                   static_cast<const Uint8*>(packed) + v * packedStride + varying.packedOffsetBytes,
                                   varying.byteSize);
                        }
                    }

                    if (MG_Pipe::gMGPipeCallbacks.OnBufferWriteback != nullptr) {
                        // Sliced for the reason the readback path above is sliced: the bytes
                        // are INLINE in a SEG_EVENT record and the ring refuses a record wider
                        // than half its capacity outright, so one whole-range post of a
                        // capture over ~128 KiB was Fatal{EventRingOverflow} and the server
                        // died. Only the EVENT is cut; the glBufferSubData below stays one
                        // call over the whole `staged` vector, because the ES store has no
                        // ring and cutting it would change how many backend calls one
                        // application call makes.
                        const SizeT step = XfbWritebackSliceStep(rangeBytes);
                        for (SizeT off = 0; off < rangeBytes; off += step) {
                            const SizeT bytes = std::min(step, rangeBytes - off);
                            MG_Pipe::gMGPipeCallbacks.OnBufferWriteback(
                                splitRes, target.start + off,
                                MG_Pipe::MGPBlobRef{reinterpret_cast<Uint64>(staged.data() + off),
                                                    static_cast<Uint64>(bytes), MG_Pipe::kMGHostSpanSegNone, 0});
                        }
                    } else {
                        MGLOG_E_ONCE("EndTransformFeedback: no reverse channel is installed, so the "
                                     "scattered capture of buffer {%u,%u} cannot reach the client shadow",
                                     splitRes.Slot, splitRes.Gen);
                    }
                    // P8-C: the staged copy takes the composed range too, so this side's CPU
                    // readers see the capture (before P8 it kept the pre-capture bytes).
                    BufferImpl::StageServerWrittenRange(*splitResource, splitRes, staged.data(), target.start,
                                                        rangeBytes);
                    // Ops_H_Readback's stamp, for its reason: the GL store now holds the
                    // capture, and the next draw must not re-upload an older range over it.
                    splitResource->syncedChangeSerial = BufferImpl::ResourceSerialForHandle(splitRes);
                    // Serial bumped with no backend op (see ReadbackCapturedRanges).
                    BufferImpl::BumpBufferMutationEpoch();
                    if (g_GLESFuncs.glBufferSubData != nullptr) {
                        BufferImpl::BindBufferId(BufferImpl::TempBufferTarget, target.backendId);
                        g_GLESFuncs.glBufferSubData(BufferImpl::TempBufferTarget, static_cast<GLintptr>(target.start),
                                                    static_cast<GLsizeiptr>(rangeBytes), staged.data());
                    }
                    BufferImpl::BindBufferId(BufferImpl::TempBufferTarget, g_scatterBufferId);
                }
                g_GLESFuncs.glUnmapBuffer(BufferImpl::TempBufferTarget);
                xfb.targets.clear();
            }
        } // namespace

        Bool AreTransformFeedbacksSupported() {
            return g_GLESFuncs.glBeginTransformFeedback != nullptr && g_GLESFuncs.glEndTransformFeedback != nullptr &&
                   g_GLESFuncs.glTransformFeedbackVaryings != nullptr;
        }

        Bool IsCaptureSpanOpen() {
            const auto& xfb = CurrentXfb();
            return (xfb.pending || xfb.started) && !xfb.paused;
        }

        void BeginTransformFeedback(GLenum primitiveMode) {
            if (!AreTransformFeedbacksSupported()) return;
            auto& xfb = CurrentXfb();
            xfb.primitiveMode = primitiveMode;
            xfb.pending = true;
            xfb.started = false;
            xfb.paused = false;
            xfb.targets.clear();
        }

        void EndTransformFeedback() {
            auto& xfb = CurrentXfb();
            const Bool wasPending = xfb.pending;
            xfb.pending = false;
            xfb.paused = false;
            if (!xfb.started) {
                // A span that never drew is legal and captures nothing by definition; one that
                // is STILL pending here drew nothing the backend saw, which for a span the
                // application expected data from is the whole bug in one line.
                MGLOG_D("EndTransformFeedback: closing a span the driver never opened (pending=%d)",
                        wasPending ? 1 : 0);
                return;
            }
            xfb.started = false;
            g_GLESFuncs.glEndTransformFeedback();
            if (const GLenum endError = TakeXfbDriverError(); endError != GL_NO_ERROR) {
                MGLOG_E_ONCE("EndTransformFeedback: the ES driver rejected glEndTransformFeedback with %s - the "
                             "driver's capture state and MobileGL's have diverged",
                             MG_Util::ConvertGLEnumToString(endError).c_str());
            }
            if (xfb.scattered) {
                ScatterCapturedRecords(xfb);
                xfb.scattered = false;
                xfb.scatterProgram.reset();
                xfb.scatterArchive.reset();
            } else {
                ReadbackCapturedRanges(xfb.targets);
            }
        }

        void PauseTransformFeedback() {
            auto& xfb = CurrentXfb();
            xfb.paused = true;
            // A span the driver never opened (paused before the first draw) has nothing to
            // pause; the flag above is what holds the deferred Begin back until the resume.
            if (!xfb.started || g_GLESFuncs.glPauseTransformFeedback == nullptr) return;
            g_GLESFuncs.glPauseTransformFeedback();
        }

        void ResumeTransformFeedback() {
            auto& xfb = CurrentXfb();
            xfb.paused = false;
            if (!xfb.started || g_GLESFuncs.glResumeTransformFeedback == nullptr) return;
            g_GLESFuncs.glResumeTransformFeedback();
        }

        void BindTransformFeedback(GLuint name) {
            auto& roleState = ActiveXfbState();
            auto& g_currentXfbState = roleState.Cached;
            auto& g_xfbObjects = roleState.Objects;
            auto& g_currentXfbName = roleState.Current;
            const Uint64 key = MG_Pipe::MGPipeApplier().BoundStreamOutputLifetimeId;
            (void)name;
            roleState.NeedsBind = false;
            g_currentXfbState = nullptr; // name changes; operator[] below may also rehash
            // The capture buffer bindings are the OBJECT's, not the context's: the bind below
            // swaps all of them for whatever the target object holds, which the redundant-bind
            // shadow has never seen.
            BufferImpl::InvalidateTransformFeedbackBindingShadows();
            if (!AreTransformFeedbackObjectsSupported()) {
                // Without driver objects there is only the default span; keep the frontend
                // name so the bookkeeping below stays consistent.
                g_currentXfbName = key;
                return;
            }
            auto& xfb = g_xfbObjects[key];
            // Even each virtual context's name-0 object gets its own native object.
            if (xfb.esId == 0) {
                g_GLESFuncs.glGenTransformFeedbacks(1, &xfb.esId);
            }
            g_GLESFuncs.glBindTransformFeedback(GL_TRANSFORM_FEEDBACK, xfb.esId);
            g_currentXfbName = key;
        }

        void DeleteTransformFeedback(GLuint name) {
            auto& roleState = ActiveXfbState();
            auto& g_currentXfbState = roleState.Cached;
            auto& g_xfbObjects = roleState.Objects;
            auto& g_currentXfbName = roleState.Current;
            const Uint64 key = MG_Pipe::MGPipeApplier().VerbDeleteStreamOutputLifetimeId;
            (void)name;
            // A generated but never-bound name has no native object or lifetime.
            if (key == 0) return;
            const auto it = g_xfbObjects.find(key);
            if (it == g_xfbObjects.end()) return;
            if (it->second.esId != 0 && g_GLESFuncs.glDeleteTransformFeedbacks != nullptr) {
                g_GLESFuncs.glDeleteTransformFeedbacks(1, &it->second.esId);
            }
            g_currentXfbState = nullptr; // erase shifts the probe cluster, moving other entries
            g_xfbObjects.erase(it);
            // The frontend reverts to the default object when the bound one is deleted.
            if (g_currentXfbName == key) {
                // The following BindStreamOutput names this context's actual
                // default lifetime. Never create a native object for key 0.
                g_currentXfbName = 0;
                roleState.NeedsBind = true;
                BufferImpl::InvalidateTransformFeedbackBindingShadows();
                return;
            }
        }

        // The ES context went away (or is being torn down): the spans, their buffer ids, the
        // driver objects and the frontend objects they pinned all belonged to it.
        void OnBackendContextDestroyed() {
            for (auto& state : g_roleXfbState)
                state = {};
            g_currentXfbState = nullptr;
            g_xfbObjects.clear();
            g_currentXfbName = 0;
            g_scatterBufferId = 0;
            g_scatterBufferSize = 0;
            BufferImpl::InvalidateTransformFeedbackBindingShadows();
        }
    } // namespace XfbImpl

    namespace VertexArrayImpl {
        // THE CLIENT-MEMORY VERTEX ARRAY UPLOAD, and the one read of this family that P5e does
        // NOT retire - it MOVES the decision instead, which is what ID-82 / ruling 2 settles.
        //
        // The bytes of a client-memory array exist only in the application's own memory: the
        // emitter publishes Res == kMGPipeNullHandle for such an attribute by design
        // (VertexInputEmit.h) and this upload is where the server used to dereference
        // attrib.Offset as a raw client pointer (Managers.cpp's
        // SyncClientSideAttributesForDrawArrays, kimi audit row 14). Under run-ahead those bytes
        // are movable and the read is torn by construction, so the CLIENT refuses the draw by
        // name before it is ever emitted (EmitTables.cpp, Fatal{UnmigratedVerb,
        // "DrawArrays+CLIENT_ARRAYS"}); staging them as a record tail is P8's.
        //
        // What is left here is therefore reachable in exactly two situations, and both are safe:
        //   * monolith / the push-monolith build  - one process, one thread, no wire;
        //   * a LOCKSTEP split session            - the client is parked in WaitForApplied for
        //                                           this very record, so its memory is stable
        //                                           (rule F is scoped to unbarriered records,
        //                                           ruling 4). Escalation (ii) of §2.1 pins the
        //                                           same thing from the other side: a draw
        //                                           carrying kDrawClientArrays is BARRIERED.
        // Making it monolith-only instead would silently drop the attribute upload of a
        // lockstep split session, which is a wrong picture today - before ra has landed
        // anything - so the refusal is the client's to raise, not this arm's to assume.
        //
        // The frontend VAO is taken ONLY when the applier's own vertex-buffer window says a
        // client-sourced attribute exists, which is what retires the per-draw
        // GetBoundVertexArray row of the ordinary (VBO-backed) DrawArrays - the strict lane's
        // `GetBoundVertexArray@DrawArrays` marker - without touching this path's behaviour.
        // `static` on purpose (G1): the pull build must not gain an exported name for a
        // function that only re-homes two identical inline blocks the two DrawArrays entry
        // points used to carry. The same spelling governs the three below, and for a second
        // reason on top of the symbol count: a Debug build drops CXX_VISIBILITY_PRESET to
        // `default` (CMakeLists.txt:707-719), so an external-linkage helper here would be a
        // DYNAMIC export in exactly the configuration a symbol gate cannot see it in.
        //
        // ALL FLAVORS, NOT JUST THE SPLIT ARM. These three were inside
        // `#if MOBILEGL_BUILD_DISAGGREGATED` for one round and that was a misreading of their
        // own gate: the body's first statement returns early when `MG_Config::Transport !=
        // Monolith`, i.e. they exist FOR the monolith arm - and the monolith arm is what the
        // pull/verify/push builds run. Their callers sit in entry points that compile in every
        // flavor (the indexed family, both indirect executors), so a guard here breaks the
        // pull build outright rather than merely omitting a split-arm path. In those flavors
        // the guard's own test folds to a compile-time false (Config.h:566) and the body is
        // the transport-free upload it always was.
        //
        // ---- THE MONOLITH ARM'S CLIENT-MEMORY SNAPSHOT, PER DRAW -----------------------------
        //
        // A client-memory attribute is uploaded HERE because its bytes have no store on this side
        // to bind. SyncClientSideVertexArraysForDrawArrays below does that for the non-indexed
        // family, whose fetched elements are exactly the contiguous (first, count) run. Every
        // OTHER draw shape fetches by index and by instance, so its elements have to come from
        // MGPipeClientFetchPlan - the same answer the wire arm's owned snapshot is built from
        // (MG_Impl/Pipe/OwnedDrawInputs.h, which is why the two arms agree by construction).
        //
        // WHAT WAS MISSING BEFORE THIS: an indexed draw over a client array uploaded nothing at
        // all. The ES context kept whatever the last VBO-backed attribute had left for that
        // index - normally the array DISABLED, i.e. every vertex reading the generic current
        // value - so the draw painted nothing, with no GL error anywhere to say why.
        struct ClientSnapshotSources {
            const SharedPtr<MG_State::GLState::BufferObject>* ElementBuffer = nullptr;
        };
    } // namespace VertexArrayImpl
} // namespace MobileGL::MG_Backend::DirectGLES
