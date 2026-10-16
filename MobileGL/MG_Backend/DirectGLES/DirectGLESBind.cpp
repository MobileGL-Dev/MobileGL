// MobileGL - MobileGL/MG_Backend/DirectGLES/DirectGLESBind.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "DirectGLESInternal.h"

namespace MobileGL::MG_Backend::DirectGLES {
    namespace BufferImpl {
        // P5e (vi), CONTRACT-P5E §5.1: THE SAME MEMO, THE SAME THREE-VALUE KEY, AND A WALK THAT
        // READS NO FRONTEND AT ALL. The arm above keeps the frontend attribute walk because the
        // push-monolith build has to keep its bytes (§5.8); this one is what a server with no
        // client objects on its side runs, and the difference between them is exactly three
        // substitutions:
        //
        //   the walk       st.VertexBuffers[Start .. Start+Count) instead of
        //                  GetAllAttributes(): the SAME set, because the emitter resolves the
        //                  attributes on the client and publishes one entry per attribute slot
        //                  (VertexInputEmit.h's EmitVertexBuffers), with Res == null for a
        //                  client-memory array and for a disabled slot - which is why the null
        //                  skip below is not a hole but the record's own "nothing to ensure".
        //   the dedupe     a {slot, gen} compare instead of a raw address compare. An address
        //                  a successor object can reproduce is the identity hazard this whole
        //                  phase is about; a handle cannot be reproduced (Gen moves on reuse).
        //   the ensure     EnsureBufferResourceForHandle(nullptr, Res). The frontend argument
        //                  is only ever used for BufferObject::SyncPersistentMappedRange, which
        //                  that function already skips under a transport (D-N), so passing
        //                  nullptr removes a read rather than dropping work - and (nullptr, h)
        //                  is already the live shape of the indirect-buffer ensure two hundred
        //                  lines below.
        //
        // The IBO half lives in SyncNeccessaryBuffers beside the legacy one, for the reason it
        // always did: the index slot is not part of the configuration (D5).
        void SyncVaoAttributeBuffersByRecord(VertexArrayImpl::BackendVertexArrayObject::ResolvedDrawBuffers* memo,
                                             Uint64 bufferEpoch) {
            const auto& st = MG_Pipe::MGPipeApplier();
            const MG_Pipe::MGPipeHandle elements = st.BoundVertexElements;
            Uint64 elementsSerial = 0;
            Bool haveElementsRecord = false;
            if (!MG_Pipe::MGPipeHandleIsNull(elements) && elements.Slot < st.VertexElementsCsos.size()) {
                const auto& record = st.VertexElementsCsos[elements.Slot];
                if (record.Live && record.Gen == elements.Gen) {
                    elementsSerial = record.ContentSerial;
                    haveElementsRecord = true;
                }
            }
            const Uint64 buffersSerial = st.VertexBuffersSerial;

            // NO LIVE ELEMENTS RECORD IS A MISS, NEVER A HIT - the arm above states the whole
            // argument (MGPipeApplierReset empties the table at every context change and the
            // key would then never move again).
            const Bool memoKeyIsMeaningful = haveElementsRecord;

            if (memo && memo->valid && memoKeyIsMeaningful && memo->elementsHandle == elements &&
                memo->elementsSerial == elementsSerial && memo->buffersSerial == buffersSerial) {
                if (memo->vboCleanEpoch != bufferEpoch) {
                    Bool allClean = true;
                    for (Uint i = 0; i < memo->count; ++i) {
                        auto& entry = memo->entries[i];
                        // nullptr, not entry.frontend: the frontend question inside
                        // IsBufferDrawCleanByHandle is monolith-only already
                        // (askTheObjectWhetherItIsMapped), and a raw client address held across
                        // records is precisely what §4.4 stops the apply thread dereferencing.
                        if (IsBufferDrawCleanByHandle(entry.handle, entry.resource, nullptr)) continue;
                        allClean = false;
                        // Re-ensured BY HANDLE. The legacy repair went back through the memoed
                        // attribute index to re-read attrib.Buffer; here the entry already
                        // names the resource the record named, so the repair needs nothing the
                        // walk did not already have.
                        entry.resource = EnsureBufferResourceForHandle(nullptr, entry.handle);
                    }
                    memo->vboCleanEpoch = allClean ? bufferEpoch : 0;
                }
                return;
            }

            // Full walk, once per distinct buffer, rebuilding the memo as it goes. WHICH
            // buffers is ResolveDrawVertexBuffersFromRecord's answer and only its answer - see
            // Managers.h: that separation is what lets a unit case drive the source decision
            // this package changed without an ES context, and what makes a revert of it visible
            // rather than duplicated.
            DrawVertexBufferRequest requests[MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS];
            const Uint syncedBufferCount = ResolveDrawVertexBuffersFromRecord(
                st, requests, MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS);
            for (Uint i = 0; i < syncedBufferCount; ++i) {
                auto* resource = EnsureBufferResourceForHandle(nullptr, requests[i].Res);
                if (memo) {
                    auto& entry = memo->entries[i];
                    // Explicitly cleared rather than left as whatever the previous build of
                    // this memo stored: nothing on this arm may dereference it, and a stale
                    // address that merely happens never to be read is the kind of thing a
                    // later reader takes for a live one.
                    entry.frontend = nullptr;
                    entry.attribIndex = static_cast<Uint8>(requests[i].BindingIndex);
                    entry.resource = resource;
                    entry.handle = requests[i].Res;
                }
            }
            if (memo) {
                memo->count = syncedBufferCount;
                memo->elementsHandle = elements;
                memo->elementsSerial = elementsSerial;
                memo->buffersSerial = buffersSerial;
                memo->valid = memoKeyIsMeaningful;
                // Rebuilt via the ensure, not probed clean: the next probe pass stamps it.
                memo->vboCleanEpoch = 0;
            }
        }

        // `vaoConfigVersion` is the caller's early read of currentVAOObject->GetConfigVersion():
        // the VAO's config fields live on a cache line the draw path touches nowhere else, and
        // cycling section VAOs makes that a guaranteed miss - reading it at the top of
        // PrepareForDraw overlaps the miss with the program/texture-key work instead of
        // stalling the memo check below. Nothing between the read and here can move it
        // (only frontend GL entry points mutate VAO config, none run inside a preparation).
        void SyncNeccessaryBuffers(const SharedPtr<MG_State::GLState::VertexArrayObject>& currentVAOObject,
                                   VertexArrayImpl::BackendVertexArrayObject* vaoTwin, Uint32 vaoConfigVersion,
                                   Bool includeIBO = false, Bool includeIndirectBuffer = false) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            ProcessDeferredBufferReleases();
#if MOBILEGL_BUILD_DISAGGREGATED
            ProcessDeferredT0Retires(); // P11 B2
#endif

            // All buffers we need are:
            //   1.VBO 2.IBO (if needed) 3.UBO 4.IndirectBuffer (if needed)
            // PBO is not needed since it should be handled in frontend

            // VBO. The distinct-buffer set is memoed on the VAO's backend twin: cycling
            // hundreds of section VAOs re-walked 32 cold VertexAttribute slots per draw
            // only to rediscover the same one or two static buffers. While the config
            // version holds (every attach/enable/disable bumps it, and it pins each
            // memoed frontend pointer via the attribute SharedPtrs), the walk reduces to
            // an IsBufferDrawClean probe per distinct buffer; only dirty entries take
            // EnsureBufferResource, re-fetched through their attribute index.
            // Pre-pass epoch read (acquire), stamped into the memo only after a pass in
            // which EVERY probe came up clean: while the stamp still equals the current
            // epoch, no path that can dirty any buffer has run (see the mutation-site
            // enumeration at CurrentBufferMutationEpoch's declaration in Managers.h),
            // so the probes themselves are skipped. A mid-pass bump lands after this
            // read, makes the stamp stale, and re-runs the probes next draw.
            const Uint64 bufferEpoch = CurrentBufferMutationEpoch();
            auto* memo = vaoTwin ? &vaoTwin->GetResolvedDrawBuffersMemo() : nullptr;
            const Uint32 configVersion = vaoConfigVersion;
            SyncVaoAttributeBuffersByRecord(memo, bufferEpoch);

            // IBO, memoed by bound-object identity (its slot version is not covered by
            // the config version). A stale identity hit is impossible in effect: the
            // clean probe re-validates the resource against the LIVE bound object.
            if (includeIBO) {
                // P5e (vi), CONTRACT-P5E §5.1: the index buffer is st.IndexBuffer.Res and the
                // "has it moved" question is st.IndexBufferSerial. The identity compare stays -
                // it always did, because the index slot is outside the configuration version
                // (D5) - but it now compares a handle the RECORD named instead of a frontend
                // address the slot was probed for, and the serial joins it because this arm has
                // no live slot to re-read: see ResolvedDrawBuffers::iboSerial in Managers.h.
                //
                // A null Res is "no element buffer bound" and is a no-op here exactly as the
                // absent `possibleIBO` is below: SyncToBackendFromApplier is what binds
                // GL_ELEMENT_ARRAY_BUFFER back to 0 for such a draw, and it keys on the same
                // serial.
                const DrawIndexBufferRequest request = ResolveDrawIndexBufferFromRecord(MG_Pipe::MGPipeApplier());
                const MG_Pipe::MGPipeHandle iboHandle = request.Res;
                const Uint64 iboSerial = request.Serial;
                if (!MG_Pipe::MGPipeHandleIsNull(iboHandle)) {
                    const Bool identityHolds =
                        memo != nullptr && memo->iboHandle == iboHandle && memo->iboSerial == iboSerial;
                    if (identityHolds && memo->iboCleanEpoch == bufferEpoch) {
                        // probed fully clean at this epoch; nothing can have dirtied it
                    } else if (identityHolds && IsBufferDrawCleanByHandle(iboHandle, memo->iboResource, nullptr)) {
                        memo->iboCleanEpoch = bufferEpoch;
                    } else {
                        auto* resource = EnsureBufferResourceForHandle(nullptr, iboHandle);
                        if (memo) {
                            memo->iboHandle = iboHandle;
                            memo->iboSerial = iboSerial;
                            memo->iboFrontend = nullptr; // see Entry::frontend's note
                            memo->iboResource = resource;
                            // Repaired, not probed clean: stamp on the next clean probe.
                            memo->iboCleanEpoch = 0;
                        }
                    }
                }
            }

            // Indirect Buffer Object - must also be bound to GL_DRAW_INDIRECT_BUFFER on the ES
            // context since indirect draws now execute natively on the GPU.
            if (includeIndirectBuffer) {
                // P5c (hd, CONTRACT-P5C §3.5): under an active transport the buffer is the verb
                // record's handle; the frontend binding slot and the client allocator are
                // never read (T2). The twin's ensure is what SyncBoundBuffer's did.
                const MG_Pipe::MGPipeHandle handle = MG_Pipe::MGPipeApplier().VerbIndirectBuffer;
                if (!MG_Pipe::MGPipeHandleIsNull(handle)) {
                    auto* resource = EnsureBufferResourceForHandle(nullptr, handle);
                    if (resource && resource->id != 0) {
                        BindBufferId(GL_DRAW_INDIRECT_BUFFER, resource->id);
                    }
                }
            }

            // UBO binding points are (re)established per draw by BindCurrentProgramWithResources at
            // their compacted link-time points: CacheResourceLocations glUniformBlockBinding's the
            // transpiled ESSL blocks to points 0,1,2,... (layout(binding=N) is stripped from the
            // ESSL), so those compacted points are the only ones the shader reads. A frontend-indexed
            // sync here would bind points the shader never reads and is unconditionally overwritten by
            // the program rebind that always follows in PrepareForDraw - i.e. redundant for draws - so
            // it is intentionally omitted (see SyncComputeBuffers for the compute path, which needs it).
            // SSBOs are different: their block bindings are baked into the ESSL at compile time and
            // BindCurrentProgramWithResources binds no SSBO points, so this is their sole draw-path
            // binder (e.g. Flywheel's indirect vertex shaders pull instance data from storage buffers).
            // P5e (sb, §5.6): the record arm, selected by transport + bit 13. Same `if (…) else`
            // shape the indirect buffer above already uses.
            SyncBufferBindingPointsByRecord(MG_Pipe::kMGPipeShaderBufferClassShaderStorage, GL_SHADER_STORAGE_BUFFER);
        }
    } // namespace BufferImpl

    namespace XfbImpl {
        namespace {
            // Binds a scratch buffer, sized for `capacityVertices` gap-free records, to
            // capture point 0 in place of the application's buffers. Returns false when the
            // scratch storage cannot be provided, in which case the caller falls back to the
            // direct binding (which produces a wrong layout, but is what happened before).
            Bool BindScatterCaptureBuffer(SizeT packedStride, SizeT capacityVertices) {
                auto& g_scatterBufferId = CurrentXfb().serverScatterBuffer;
                auto& g_scatterBufferSize = CurrentXfb().serverScatterSize;
                if (packedStride == 0 || capacityVertices == 0) return false;
                if (g_GLESFuncs.glGenBuffers == nullptr || g_GLESFuncs.glBufferData == nullptr) return false;
                const SizeT required = packedStride * capacityVertices;
                if (g_scatterBufferId == 0) {
                    g_GLESFuncs.glGenBuffers(1, &g_scatterBufferId);
                    if (g_scatterBufferId == 0) return false;
                    g_scatterBufferSize = 0;
                }
                if (g_scatterBufferSize < required) {
                    BufferImpl::BindBufferId(BufferImpl::TempBufferTarget, g_scatterBufferId);
                    g_GLESFuncs.glBufferData(BufferImpl::TempBufferTarget, static_cast<GLsizeiptr>(required), nullptr,
                                             GL_DYNAMIC_COPY);
                    g_scatterBufferSize = required;
                }
                // Point 0 carries every captured varying: the gl_NextBuffer / gl_SkipComponents
                // entries are consumed at link time and never reach the driver, so the ES
                // program is declared INTERLEAVED over a single buffer and point 0 is the only
                // point it can write (GL 4.6 core 13.2.2).
                //
                // The other points are therefore left exactly as they are. Clearing them - which
                // this used to do, across the application's whole touched high-water mark - is
                // both unnecessary (the ES program cannot write an unused point) and the precise
                // trigger for the Mali G1-Ultra capture loss: see
                // SyncTransformFeedbackBindingPoints for the mechanism and the device evidence.
                // KHR-GL46.transform_feedback.capture_special_interleaved_test is the case that
                // reaches this path.
                BufferImpl::BindBufferRangeCached(GL_TRANSFORM_FEEDBACK_BUFFER, 0, g_scatterBufferId, 0,
                                                  static_cast<GLsizeiptr>(required));
                return true;
            }
        } // namespace

        // Tail of PrepareForDraw: the program is bound and every buffer the draw needs
        // is up to date, so the capture buffers can be bound and the span opened.
        void StartPendingTransformFeedback() {
            auto& xfb = CurrentXfb();
            // A span that was paused before its first draw must not open here: the draw is
            // not captured, and opening the span would also subject it to the capture
            // primitive-mode rule the paused draw is exempt from.
            if (!xfb.pending || xfb.paused) return;
            XfbProgramSource program;
            const MG_Pipe::MGPStreamOutputBegin* span = nullptr;
            const auto& state = MG_Pipe::MGPipeApplier();
            const auto found = state.StreamOutputSpans.find(state.BoundStreamOutputLifetimeId);
            if (found != state.StreamOutputSpans.end()) {
                span = &found->second;
                const auto* record = PipeShaderCsoRecordForHandle(span->CaptureProgram);
                if (record) program.Archive = record->Archive;
            }
            if (!program) {
                // The pending flag is deliberately NOT consumed here. It used to be cleared
                // before this check, so a single draw that could not see the capture program
                // retired the span permanently: every later draw of the same span found
                // pending==false, the driver Begin never happened, and End found started==false
                // and skipped the readback - a whole capture lost with no GL error anywhere.
                // The frontend only reaches a draw with an active span after glBeginTransformFeedback
                // stored a program, so this is a "cannot happen" that must stay recoverable.
                MGLOG_E_ONCE("StartPendingTransformFeedback: an active capture span has no capture program; the "
                             "driver span stays closed and this draw is not captured");
                return;
            }
            xfb.pending = false;

            // Snapshot what the driver is about to capture into. GL forbids rebinding the
            // capture buffers while the span is open, so this stays valid until End, and
            // recording it here keeps End independent of the frontend capture state.
            const SizeT bufferCount = program->GetTransformFeedbackBufferCount();
            for (SizeT i = 0; i < bufferCount; ++i) {
                if (!span || i >= 4) continue;
                const auto& range = span->Targets[i];
                if (MG_Pipe::MGPipeHandleIsNull(range.Res) || range.Size == 0) continue;
                auto* resource = BufferImpl::EnsureBufferResourceForHandle(nullptr, range.Res);
                if (!resource || resource->id == 0) continue;
                xfb.targets.push_back({nullptr, resource->id, static_cast<SizeT>(range.Offset),
                                       static_cast<SizeT>(range.Offset + range.Size), i, range.Res});
                BufferImpl::BindBufferRangeCached(GL_TRANSFORM_FEEDBACK_BUFFER, static_cast<GLuint>(i), resource->id,
                                                  static_cast<GLintptr>(range.Offset),
                                                  static_cast<GLsizeiptr>(range.Size));
                continue;
            }

            // A layout with holes or several interleaved buffers is not expressible on ES:
            // capture gap-free into scratch storage and place the records at End instead.
            xfb.scattered = false;
            xfb.scatterProgram.reset();
            xfb.scatterArchive.reset();
            xfb.scatterCapacityVertices = 0;
            if (program->NeedsScatteredTransformFeedbackCapture()) {
                SizeT capacityVertices = ~SizeT(0);
                for (const auto& target : xfb.targets) {
                    // By BUFFER index. Reading the stride at the target's POSITION made a
                    // capture list beginning with gl_NextBuffer - buffer 0 has stride 0 and
                    // nothing bound, so target 0 describes buffer 1 - read stride 0, skip every
                    // target, and leave the capacity at zero.
                    const SizeT stride = program->GetTransformFeedbackStride(static_cast<Uint32>(target.bufferIndex));
                    if (stride == 0) continue;
                    capacityVertices = std::min<SizeT>(capacityVertices, (target.end - target.start) / stride);
                }
                if (capacityVertices == ~SizeT(0)) capacityVertices = 0;
                if (BindScatterCaptureBuffer(program->GetTransformFeedbackPackedStride(), capacityVertices)) {
                    xfb.scattered = true;
                    xfb.scatterProgram = program.Frontend;
                    xfb.scatterArchive = program.Archive;
                    xfb.scatterCapacityVertices = capacityVertices;
                } else {
                    // NO SPAN RATHER THAN A SPAN THAT WRITES SOMEWHERE ELSE. The ES program for a
                    // scattered capture is a single-buffer INTERLEAVED one (the gl_NextBuffer /
                    // gl_SkipComponents entries are consumed at link time and never reach the
                    // driver), so it writes capture point 0 and nothing else. Point 0 here is
                    // either unbound or - the dangerous case - still holds whatever an earlier
                    // capture in this process bound there, because the frontend's own
                    // glBindBufferBase is state-only and nothing else in the backend touches the
                    // indexed points. Opening the span would then have the driver capture over an
                    // application buffer that has nothing to do with this draw, and the frontend
                    // shadow would never learn of it.
                    //
                    // Leaving the span closed reproduces exactly what the old high-water clear
                    // loop achieved by binding 0 here and letting the driver refuse the Begin -
                    // the capture records nothing - without issuing a capture-point bind the
                    // application did not ask for, which is the thing that loses captures whole
                    // on Mali (see SyncTransformFeedbackBindingPoints).
                    MGLOG_E_ONCE("StartPendingTransformFeedback: no scratch storage for a scattered capture "
                                 "(packed stride %zu, capacity %zu vertices); leaving the driver span CLOSED so the "
                                 "capture cannot land in a stale binding. Nothing will be captured.",
                                 program->GetTransformFeedbackPackedStride(), capacityVertices);
                    xfb.targets.clear();
                    return;
                }
            }

            // A capture program with buffers bound must have produced at least one target;
            // an empty list means End has nothing to mirror back and the application will
            // read its buffer's pre-draw bytes however well the GPU captured.
            if (xfb.targets.empty()) {
                MGLOG_E_ONCE("StartPendingTransformFeedback: opening a capture span with NO capture targets "
                             "(program declares %zu capture buffer(s), none of them resolved to a bound backend "
                             "buffer with a non-empty range); nothing will be read back",
                             bufferCount);
            }

            g_GLESFuncs.glBeginTransformFeedback(xfb.primitiveMode);
            // Unchecked before. Every ES error condition here (already active, a current
            // program with no capture set, a capture point the program uses with no buffer)
            // ends the same way: the driver records nothing, GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN
            // reads 0 and the application sees no error at all - MobileGL's own error state is
            // separate from the driver's, so a driver rejection here is invisible to it.
            if (const GLenum beginError = TakeXfbDriverError(); beginError != GL_NO_ERROR) {
                // The mode is printed as a number as well as a name: GL_POINTS is 0, which the
                // enum converter spells "GL_FALSE", and a reader chasing a lost capture should
                // not have to know that.
                MGLOG_E_ONCE("StartPendingTransformFeedback: the ES driver REJECTED "
                             "glBeginTransformFeedback(%s / 0x%04x) with %s - nothing will be captured. Backend "
                             "program %u, %zu capture buffer(s), %zu target(s), mode=%s.",
                             MG_Util::ConvertGLEnumToString(xfb.primitiveMode).c_str(),
                             static_cast<unsigned>(xfb.primitiveMode),
                             MG_Util::ConvertGLEnumToString(beginError).c_str(), PrgramImpl::g_lastUsedBackendProgramId,
                             bufferCount, xfb.targets.size(),
                             MG_Util::ConvertGLEnumToString(program->GetTransformFeedbackBufferMode()).c_str());
            }
            xfb.started = true;
        }
    } // namespace XfbImpl

    namespace VertexArrayImpl {
        // P5e (vi): the record arm's entry, and it takes NO frontend object - CONTRACT-P5E §4.1
        // ("the object parameter is deleted from the transport overload, not defaulted to
        // null"). A null twin here is the null BoundVertexElements, i.e. "no VAO bound", which
        // PrepareForDraw turns into BindBackendVAOId(0) exactly as a null frontend VAO did;
        // logging about it would fire on every draw of a session that legitimately has no
        // vertex-elements CSO yet.
        void SyncCurrentVAOFromRecords(BackendVertexArrayObject* vaoTwin) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (!vaoTwin) return;
            vaoTwin->SyncToBackendFromApplier();
        }

        // GL: a shader input whose generic attribute array is DISABLED reads that attribute's *current
        // value* (context state set by glVertexAttrib*, default (0,0,0,1)) rather than any buffer.
        // MobileGL stores those values in MG_State only, so without this step the ES driver would feed
        // the shader its own current values, which MobileGL never writes -- i.e. always (0,0,0,1).
        // SyncToBackend has already issued glDisableVertexAttribArray for these locations, so the ES
        // current value is what the shader will actually read.
        void SyncCurrentVertexAttributeValues(BackendVertexArrayObject* vaoTwin,
                                              const SharedPtr<MG_State::GLState::ProgramObject>& program) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            if (!program) return;
            if (!vaoTwin) return;

            // P5e (vi), CONTRACT-P5E §5.1: the bound-VAO row is retired here. On the record arm
            // the twin IS the bound vertex-elements CSO (the caller resolved it from
            // st.BoundVertexElements), so "is a VAO bound" is already answered by vaoTwin being
            // non-null and GetBoundVertexArray() has nothing left to add.
            //
            // WHAT IS NOT RETIRED HERE, and it is S4/pg's not vi's: the two program reads below
            // (GetActiveAttributeLocationMask, GetAttribType). They are frontend object rows of
            // the PROGRAM family and stay until pg carries the attribute mask and types in the
            // ShaderCso record. Until then this function is still a frontend reader on the
            // apply thread - a BARRIERED one, which is legal (§4.4 / ruling 4) and is why vi
            // does its half now rather than waiting: the VAO half is what this package owns and
            // leaving it would keep GetBoundVertexArray alive for a reason that is not the
            // program's.
            const MG_Pipe::MGPipeVertexElementsRecord* elementsRecord = nullptr;
            MG_Pipe::MGPipeHandle elementsHandle = MG_Pipe::kMGPipeNullHandle;
            Uint64 elementsSerial = 0;
            const auto& st = MG_Pipe::MGPipeApplier();
            elementsHandle = st.BoundVertexElements;
            if (!MG_Pipe::MGPipeHandleIsNull(elementsHandle) && elementsHandle.Slot < st.VertexElementsCsos.size()) {
                const auto& record = st.VertexElementsCsos[elementsHandle.Slot];
                if (record.Live && record.Gen == elementsHandle.Gen) {
                    elementsRecord = &record;
                    elementsSerial = record.ContentSerial;
                }
            }
            // No record is "the configuration was never described": there is nothing to
            // decide which locations lack an array from, and guessing would either
            // re-issue every current value every draw or silently skip the ones that need
            // it. The next create_vertex_elements re-opens the memo by serial.
            if (elementsRecord == nullptr) return;

            const SharedPtr<MG_State::GLState::VertexArrayObject> noVao;
            const auto& vao = noVao;

            const Uint32 activeAttribMask = program->GetActiveAttributeLocationMask();
            if (activeAttribMask == 0) return;

            // Which of the program's attributes lack an enabled array cannot change without
            // the VAO's config version moving (Enable/DisableAttribute bump it) or the
            // program's active mask changing, so the per-draw Enabled probes reduce to two
            // compares. The memo lives on the twin (1:1 with the VAO, no identity key
            // needed): a function-static single entry missed on every draw once the app
            // cycled section VAOs, re-reading the cold attribute slots each time; here a
            // cycle re-hits every VAO's own entry. The rebuild visits only ACTIVE locations.
            auto& memo = vaoTwin->GetPendingAttribValueMaskMemo();
            // The re-keyed memo: {elementsHandle, ContentSerial, activeMask}. Every
            // Enable/DisableVertexAttribArray moves the frontend configuration version, the
            // client re-emits create_vertex_elements on the same handle for it, and the
            // applier ++s ContentSerial - so this key opens exactly when the old one did
            // and never wraps (Managers.h states the strictly-stronger argument).
            if (!memo.valid || !(memo.elementsHandle == elementsHandle) || memo.elementsSerial != elementsSerial ||
                activeAttribMask != memo.activeMask) {
                Uint32 pending = 0;
                for (Uint32 remaining = activeAttribMask; remaining != 0; remaining &= remaining - 1) {
                    const Uint32 location = static_cast<Uint32>(std::countr_zero(remaining));
                    // rec->Attributes[] IS what was last pushed for this configuration; the
                    // record carries all 32 slots and zeroes the tail
                    // (MGPipeApplyCreateVertexElements), so a location past AttributeCount
                    // reads Enabled = 0, which is the same answer the frontend's cold
                    // attribute slot gave.
                    if (location >= MG_Pipe::kMGPipeMaxVertexAttribs || !elementsRecord->Attributes[location].Enabled) {
                        pending |= (1u << location);
                    }
                }
                memo.elementsHandle = elementsHandle;
                memo.elementsSerial = elementsSerial;
                memo.activeMask = activeAttribMask;
                memo.pendingMask = pending;
                memo.valid = true;
            }
            // Plus the enabled arrays the walk had to disable (Managers.h, GetDisabledEnabledAttribMask).
            const Uint32 pendingMask = memo.pendingMask | (vaoTwin->GetDisabledEnabledAttribMask() & memo.activeMask);
            if (pendingMask == 0) return;

            for (Uint32 remaining = pendingMask; remaining != 0; remaining &= remaining - 1) {
                const Uint32 location = static_cast<Uint32>(std::countr_zero(remaining));

                const auto& currentValue = MG_Pipe::gPipeInputs->GetCurrentVertexAttribute(location);
                const auto typeInfo = MG_State::GLState::ClassifyVertexAttribType(program->GetAttribType(location));
                switch (typeInfo.baseType) {
                case MG_State::GLState::VertexAttribBaseType::Float:
                    g_GLESFuncs.glVertexAttrib4fv(location, currentValue.floatValue.data());
                    break;
                case MG_State::GLState::VertexAttribBaseType::Int:
                    g_GLESFuncs.glVertexAttribI4iv(location, currentValue.intValue.data());
                    break;
                case MG_State::GLState::VertexAttribBaseType::Uint:
                    g_GLESFuncs.glVertexAttribI4uiv(location, currentValue.uintValue.data());
                    break;
                case MG_State::GLState::VertexAttribBaseType::Unsupported:
                    MGLOG_E_ONCE(
                        "SyncCurrentVertexAttributeValues: program=%u location=%u has no enabled array and its "
                        "shader input type 0x%x is not supported as a current generic vertex attribute",
                        program->GetExternalIndex(), location, program->GetAttribType(location));
                    break;
                }
            }
        }

        // P5e (pa), CONTRACT-P5E §5.5 / §5.8 (ruling ID-81): THE SAME SYNC, OFF THE RECORD.
        //
        // The overload above is the MONOLITH GLUE and keeps its text token for token; this one is
        // what the handle arm calls, and it answers the note vi left inside it - "the two program
        // reads below (GetActiveAttributeLocationMask, GetAttribType) ... stay until pg carries
        // the attribute mask and types in the ShaderCso record". pg's archive carries both:
        // `attribs` and `attribTypes` are members of LinkArtifacts, the record ADOPTS the whole of
        // it at create_shader_state, and neither is one of the three post-link mutable fields the
        // bindings tails exist for (glUniformBlockBinding / glUniform1i on a sampler /
        // glShaderStorageBlockBinding move those, and nothing moves these without a relink, which
        // re-issues the create). So this arm adds NOTHING to the wire and changes no record - it
        // reads rows that already arrived.
        //
        // WHY A SECOND FUNCTION rather than an `#if` inside the body above: §5.8's idiom, and G1 -
        // the frontend overload's preprocessed text does not move, so the pull build gains no
        // symbol and no byte. The price is that vi's memo arm is COPIED rather than shared; a
        // shared tail would have rewritten the pull build's one, which G1 measures at 0/0/0/0.
        void SyncCurrentVertexAttributeValues(BackendVertexArrayObject* vaoTwin, MG_Pipe::MGPipeHandle cso) {
#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
            // THE SAME THREE-PART TEST SyncCurrentProgramByHandle MAKES, from the descriptor, and
            // it is this arm's spelling of `if (!program) return;` above rather than a weaker one.
            // LinkStatus is a FIELD and not an implication (ID-88): create_shader_state is
            // re-issued at every link that moves the link version and a FAILED relink of a bound
            // program moves it too, so "a record exists" and "the program linked" are different
            // statements. A program that did not link, or whose SPIR-V never arrived, is one the
            // same Prepare has just bound program 0 for - there is no shader to feed a current
            // generic attribute to, which is what the frontend arm gets from an unlinked
            // program's empty `attribs`.
            const MG_Pipe::MGPipeShaderCsoRecord* const record = PipeShaderCsoRecordForHandle(cso);
            if (record == nullptr || record->Desc.LinkStatus == 0 || record->Desc.SpirvStatus == 0) {
                return;
            }
            // A NULL ARCHIVE ON THIS ARM IS A NAMED REFUSAL, NEVER A FALL-BACK TO THE FRONTEND -
            // the shape of RefuseNullFrontendTextureOffTheHandleArm / MGB_TEXTURE_RECORD_ARM_-
            // SELECTED (Managers.cpp, ID-110). This function is reached from ProgramHandleArm()
            // only, i.e. Transport != Monolith AND the program subsystem bit, and under a
            // transport pg fills Archive at every create_shader_state; so a LIVE record with a
            // LINKED descriptor and no archive is a seam defect and not a state a draw may run
            // in. Reaching back for GetProgramForDraw() here would answer the two rows out of the
            // client's own live LinkArtifacts - the memory Link() replaces in place, i.e. exactly
            // the read this package retires - and would do it behind a picture that still looks
            // right, which is what the subsystem A/B exists to expose.
            if (!record->Archive) {
                MGLOG_F("MGPipe: Fatal{RoleViolation, \"program-handle-arm\"} - "
                        "SyncCurrentVertexAttributeValues needs the active-attribute mask and the "
                        "attribute types of ShaderCso {%u, %u} and that record carries no "
                        "server-owned archive. The arm is selected by Transport != Monolith AND "
                        "the program subsystem bit (CONTRACT-P5E §5.8, ID-81), so this record's "
                        "create_shader_state was applied under a transport and must have adopted "
                        "one; the frontend ProgramObject is not a fall-back here, it IS the client "
                        "memory this row retires",
                        cso.Slot, cso.Gen);
                std::abort();
            }
            const MG_State::GLState::LinkArtifacts& link = record->Archive->Link;
            if (!vaoTwin) return;

            // vi's half, unchanged in meaning and copied rather than shared (see above). The two
            // family bits are independent A/Bs, so a program on the handle arm says nothing about
            // whether the vertex-input family is - this arm has to carry both of vi's.
            const MG_Pipe::MGPipeVertexElementsRecord* elementsRecord = nullptr;
            MG_Pipe::MGPipeHandle elementsHandle = MG_Pipe::kMGPipeNullHandle;
            Uint64 elementsSerial = 0;
            const auto& st = MG_Pipe::MGPipeApplier();
            elementsHandle = st.BoundVertexElements;
            if (!MG_Pipe::MGPipeHandleIsNull(elementsHandle) && elementsHandle.Slot < st.VertexElementsCsos.size()) {
                const auto& elements = st.VertexElementsCsos[elementsHandle.Slot];
                if (elements.Live && elements.Gen == elementsHandle.Gen) {
                    elementsRecord = &elements;
                    elementsSerial = elements.ContentSerial;
                }
            }
            if (elementsRecord == nullptr) return;

            const SharedPtr<MG_State::GLState::VertexArrayObject> noVao;
            const auto& vao = noVao;

            // ProgramObject::GetActiveAttributeLocationMask, over the archive's own `attribs`:
            // the same 32-location bound and the same "an empty name is not an active attribute"
            // rule, computed where the names live instead of through an accessor MG_Backend
            // cannot reach without a frontend object.
            Uint32 activeAttribMask = 0;
            {
                const SizeT attribCount = std::min<SizeT>(link.attribs.size(), 32);
                for (SizeT index = 0; index < attribCount; ++index) {
                    if (!link.attribs[index].empty()) activeAttribMask |= (1u << index);
                }
            }
            if (activeAttribMask == 0) return;

            auto& memo = vaoTwin->GetPendingAttribValueMaskMemo();
            if (!memo.valid || !(memo.elementsHandle == elementsHandle) || memo.elementsSerial != elementsSerial ||
                activeAttribMask != memo.activeMask) {
                Uint32 pending = 0;
                for (Uint32 remaining = activeAttribMask; remaining != 0; remaining &= remaining - 1) {
                    const Uint32 location = static_cast<Uint32>(std::countr_zero(remaining));
                    if (location >= MG_Pipe::kMGPipeMaxVertexAttribs || !elementsRecord->Attributes[location].Enabled) {
                        pending |= (1u << location);
                    }
                }
                memo.elementsHandle = elementsHandle;
                memo.elementsSerial = elementsSerial;
                memo.activeMask = activeAttribMask;
                memo.pendingMask = pending;
                memo.valid = true;
            }
            // Plus the enabled arrays the walk had to disable (Managers.h, GetDisabledEnabledAttribMask).
            const Uint32 pendingMask = memo.pendingMask | (vaoTwin->GetDisabledEnabledAttribMask() & memo.activeMask);
            if (pendingMask == 0) return;

            for (Uint32 remaining = pendingMask; remaining != 0; remaining &= remaining - 1) {
                const Uint32 location = static_cast<Uint32>(std::countr_zero(remaining));

                // BOUNDS-CHECKED, exactly as ProgramObject::GetAttribType is not: the frontend's
                // form indexes attribTypes raw because the mask it walked came off the same
                // artefacts instance, and here the two vectors arrived over a wire. A short tail
                // is an archive whose halves disagree; 0 classifies as Unsupported and says so
                // below rather than reading past the end.
                const GLenum attribType = location < link.attribTypes.size() ? link.attribTypes[location] : 0;
                const auto& currentValue = MG_Pipe::gPipeInputs->GetCurrentVertexAttribute(location);
                const auto typeInfo = MG_State::GLState::ClassifyVertexAttribType(attribType);
                switch (typeInfo.baseType) {
                case MG_State::GLState::VertexAttribBaseType::Float:
                    g_GLESFuncs.glVertexAttrib4fv(location, currentValue.floatValue.data());
                    break;
                case MG_State::GLState::VertexAttribBaseType::Int:
                    g_GLESFuncs.glVertexAttribI4iv(location, currentValue.intValue.data());
                    break;
                case MG_State::GLState::VertexAttribBaseType::Uint:
                    g_GLESFuncs.glVertexAttribI4uiv(location, currentValue.uintValue.data());
                    break;
                case MG_State::GLState::VertexAttribBaseType::Unsupported:
                    // THE HANDLE, NOT A GL NAME: the server does not know the client's program
                    // names and must not learn them (ProgramArchiveSource::Identity states the
                    // same rule for the twin's log lines).
                    MGLOG_E_ONCE("SyncCurrentVertexAttributeValues: ShaderCso {%u, %u} location=%u has no "
                                 "enabled array and its shader input type 0x%x is not supported as a current "
                                 "generic vertex attribute",
                                 cso.Slot, cso.Gen, location, attribType);
                    break;
                }
            }
        }
    } // namespace VertexArrayImpl

    namespace TextureImpl {
        static Uint g_imageSweepBackendContextGeneration = 0;
        static Bool g_imageSweepValid = false;
        // P5e (fb, ruling 7 / ID-86): the record arm's three applier serials. Held in their own
        // fields beside the frontend pair rather than reusing them, for the reason the two
        // framebuffer list keys are held apart - one field carrying two key shapes is how a
        // stale half gets compared against a live one.
        static Uint64 g_imageSweepShaderImagesSerial = 0;
        static Uint64 g_imageSweepTextureShutterSerial = 0;
        static Uint64 g_imageSweepContextSerial = 0;

        // The sweep is a glBindImageTexture per unit, so it must not run per draw: the gate is the
        // frontend's sampling-resolution generation, which TextureObjectBase::BumpShapeVersion
        // moves on exactly the shape and format changes that can force the re-mint. Deliberately
        // NOT the backend-side re-mint counter (g_attachmentBackendIdGeneration's sibling would be
        // the obvious choice): a texture that is bound ONLY to an image unit is re-minted inside
        // this very sweep, so a backend-side trigger would be bumped after the gate had already
        // declined to run it.
        //
        // P4a e3 (D-G4) leaves BOTH properties of this gate exactly as they are, and says so
        // because they are the kind of thing a later optimisation deletes:
        //
        //   1. the g_imageUnitHighWaterMark == 0 early-out is what makes every Minecraft draw
        //      pay one integer test, and it is asked BEFORE anything reads a record;
        //   2. the gate key stays the FRONTEND sampling-resolution generation and is
        //      deliberately not re-keyed onto a server-owned serial, for the reason above: a
        //      texture bound only to an image unit is re-minted INSIDE this sweep, so an epoch
        //      the server bumps would move after the gate had already declined to run it. The
        //      client's own NewShaderImages shutter mixes the same three frontend counters
        //      (texture content, texture params, the program's image-unit version), so the
        //      property is preserved on both sides by construction rather than by agreement.
        //
        // P5e (fb, ruling 7 / ID-86): PROPERTY 1 IS UNTOUCHED. PROPERTY 2 IS RE-KEYED, and the
        // argument behind it is kept rather than dropped - it is what decides WHICH four values
        // the new key may be made of.
        //
        // The frontend sampling-resolution generation cannot be read on an apply thread, so the
        // key becomes (ShaderImagesSerial, TextureShutterSerial, ContextSerial,
        // g_backendContextGeneration). Three of those are APPLIER-DERIVED - they move when the
        // client's own shutters send a new set of images, new texture content or new texture
        // parameters, i.e. at exactly the moments the frontend counter moved - and the fourth
        // is the backend's ES-context generation, which the old key already carried.
        //
        // NONE OF THEM IS A BACKEND RE-MINT COUNTER, and that is the whole of the ruling: the
        // hazard the old comment names is a texture bound ONLY to an image unit being re-minted
        // INSIDE this sweep, which would bump a backend-side epoch after the gate had already
        // declined to run. An applier serial moves when the CLIENT said something, strictly
        // before the sweep the saying provoked, so the property survives the re-key intact.
        // g_backendContextGeneration is not a counter of that kind either - it moves when the
        // ES context is rebuilt, which no sweep does.
        //
        // THE FOUR SERIALS SAY NOTHING ABOUT CONTENT, so the key carries each image texture's
        // content serial as well. A glTexSubImage* into a texture that is bound ONLY to an image
        // unit moves that texture's record Serial and none of the four: the upload stays in the
        // record's pending set, and nothing else flushes it before the draw - the sampled-unit
        // walk and the attachment lists never visit an image-only texture. The draw then
        // imageStore()s into the driver texture, and the stale upload lands on top of those
        // stores at the next sync that does visit the texture (a readback), which is the
        // ordering the application asked for in reverse (KHR-GL42.shader_image_load_store.
        // early-fragment-tests, which zeroes its image before every draw).
        //
        // Per unit, keyed on the bound HANDLE and its record's Serial: the handle carries the
        // slot's generation, so a recycled slot cannot answer with its predecessor's serial, and
        // the record Serial is the server's own monotone content version. The walk is bounded by
        // the high-water mark, so it costs nothing on the draws the first early-out already
        // returns from.
        struct ImageSweepUnitContent {
            MG_Pipe::MGPipeHandle Res = MG_Pipe::kMGPipeNullHandle;
            Uint64 Serial = 0;
        };
        static Array<ImageSweepUnitContent, MG_Pipe::kMGPipeMaxImageUnits> g_imageSweepUnitContent{};
        static Uint32 g_imageSweepUnitContentCount = 0;

        static Uint32 ImageSweepUnitContentCount(const MG_Pipe::MGPipeApplierState& st) {
            return std::min<Uint32>(static_cast<Uint32>(g_imageUnitHighWaterMark),
                                    static_cast<Uint32>(st.BoundShaderImages.size()));
        }

        static Uint64 ImageUnitContentSerial(MG_Pipe::MGPipeHandle res) {
            if (MG_Pipe::MGPipeHandleIsNull(res)) return 0;
            const auto* record = PipeTextureRecordForHandle(res);
            return record != nullptr ? record->Serial : 0;
        }

        static Bool ImageSweepContentUnchanged(const MG_Pipe::MGPipeApplierState& st) {
            const Uint32 count = ImageSweepUnitContentCount(st);
            if (count != g_imageSweepUnitContentCount) return false;
            for (Uint32 unit = 0; unit < count; ++unit) {
                const MG_Pipe::MGPipeHandle res = st.BoundShaderImages[unit].Res;
                const auto& memo = g_imageSweepUnitContent[unit];
                if (!(memo.Res == res) || memo.Serial != ImageUnitContentSerial(res)) return false;
            }
            return true;
        }

        static void StampImageSweepContent(const MG_Pipe::MGPipeApplierState& st) {
            const Uint32 count = ImageSweepUnitContentCount(st);
            for (Uint32 unit = 0; unit < count; ++unit) {
                const MG_Pipe::MGPipeHandle res = st.BoundShaderImages[unit].Res;
                g_imageSweepUnitContent[unit] = {res, ImageUnitContentSerial(res)};
            }
            g_imageSweepUnitContentCount = count;
        }

        void SyncImageTextureBindingsForDraw(const DrawTextureSyncKeys& keys) {
            if (g_imageUnitHighWaterMark == 0) return;
            const auto& st = MG_Pipe::MGPipeApplier();
            if (g_imageSweepValid && g_imageSweepShaderImagesSerial == st.ShaderImagesSerial &&
                g_imageSweepTextureShutterSerial == st.TextureShutterSerial &&
                g_imageSweepContextSerial == st.ContextSerial &&
                g_imageSweepBackendContextGeneration == g_backendContextGeneration && ImageSweepContentUnchanged(st)) {
                return;
            }
            SyncImageTextureBindings();
            // The sweep may just have landed a re-upload into a texture the draw reaches through
            // an image unit (the content clause above is one of the ways it gets here).
            if (const GLbitfield barriers = TakeInPlaceTextureUploadBarrierBits();
                barriers != 0 && g_GLESFuncs.glMemoryBarrier) {
                g_GLESFuncs.glMemoryBarrier(barriers);
            }
            g_imageSweepShaderImagesSerial = st.ShaderImagesSerial;
            g_imageSweepTextureShutterSerial = st.TextureShutterSerial;
            g_imageSweepContextSerial = st.ContextSerial;
            g_imageSweepBackendContextGeneration = g_backendContextGeneration;
            // After the sweep: it is the sync that consumed the pending set, and it may raise the
            // high-water mark the walk is bounded by.
            StampImageSweepContent(st);
            g_imageSweepValid = true;
            return;
        }
    } // namespace TextureImpl

    void BindCurrentFBO(FramebufferTarget target) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        // The handle arm reads the record instead of the binding slot, which is one of the
        // five GetFramebufferBindingSlotChecked call sites this phase retires. Everything the
        // pre-handle arm below is careful about is careful here for the same reasons and in
        // the same order: NO FAST PATH on any version (the skip is BindFramebufferId's job and
        // its shadow is where the cost actually is), and the DEFAULT framebuffer is bound
        // THROUGH THE SHADOW, never raw. What changes is only where the two questions - "which
        // framebuffer" and "is it the default one" - are answered: the record's Fbo and its
        // IsDefault byte, rather than the bound object's address and a comparison against
        // pDefaultFramebufferInfo->defaultFBO.
        if (FramebufferImpl::g_fboRecordsTrusted) {
            const MG_Pipe::MGPFramebufferState* const recordPtr = BoundFramebufferRecord(target);
            // The record's own handle is the "has this binding ever been described" test, not
            // FramebufferSerial - which MGPipeApplierReset advances whether or not anything
            // was ever emitted (see SyncCurrentFBOByRecord). The trust latch above is the
            // other half: it says the sync that ran a moment ago found these two records
            // describing these two bindings.
            // P4a decline-site F5: FLIPPED AT THE VERIFICATION ROUND to an assertion, same
            //   argument and same shape as F7 above - the trust latch implies a record with a
            //   non-null handle on both targets, and a silent decline here would look exactly
            //   like a legitimate one.
            MOBILEGL_ASSERT(recordPtr != nullptr && !MG_Pipe::MGPipeHandleIsNull(recordPtr->Fbo));
            if (recordPtr != nullptr && !MG_Pipe::MGPipeHandleIsNull(recordPtr->Fbo)) {
                const MG_Pipe::MGPFramebufferState& record = *recordPtr;
                if (record.IsDefault == 0) {
                    auto* twinEntry = FramebufferImpl::g_backendFramebufferObjects.FindByHandle(record.Fbo);
                    // P4a decline-site F6: S - already loud, and it KEPT this shape at the
                    //   verification round: binding nothing is exactly what the pre-handle arm
                    //   does in the same situation, so the two arms stay at parity.
                    if (twinEntry && *twinEntry) {
                        (*twinEntry)->Bind(target);
                    } else {
                        MGLOG_E_ONCE("No backend FBO found (maybe not synced) for the current %s FBO record, "
                                     "cannot bind FBO.",
                                     (target == FramebufferTarget::Read ? "READ" : "DRAW"));
                    }
                } else {
                    MGLOG_D("Binding default framebuffer as %s FBO",
                            (target == FramebufferTarget::Read ? "READ" : "DRAW"));
                    if (FramebufferImpl::CurrentFramebufferBinding(target) != 0) FlushAheadOfWindowBufferUse();
                    // Through the shadow: a raw bind here would leave the shadow claiming
                    // the previous user FBO, false-skipping its next re-bind.
                    FramebufferImpl::BindFramebufferId(
                        target == FramebufferTarget::Draw ? GL_DRAW_FRAMEBUFFER : GL_READ_FRAMEBUFFER, 0);
                }
                return;
            }
        }
        // P5e (fb, §5.4): the pre-handle arm below reads the binding slot, so under a transport
        // reaching it is the same refusal SyncCurrentFBO takes. Reaching it means either the
        // sync's trust latch is clear - and the sync aborted before it could be - or this bind
        // ran with no sync in front of it, which the latch exists to catch.
        RefuseFramebufferBindingSlotRead();
    }

    void PrepareForDraw(DrawSyncFlags syncBit) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        // One twin resolve serves the whole draw: the buffer sync (which hosts the
        // resolved-buffers memo on the twin), the VAO sync and the draw-time bind
        // below. Nothing in between can invalidate it — the bound VAO is pinned by
        // the context, and no step here erases or replaces a live VAO's twin.
        //
        // P5e (vi), CONTRACT-P5E §4.2 / §5.1: under a transport the twin is resolved from
        // st.BoundVertexElements - THE SAME HANDLE the frontend probe used to mint, since
        // EmitVertexElements acquires it off the VAO's own lifetime id - and neither the bound
        // VAO row nor the configuration version is read at all. `currentVAO` stays null on that
        // arm, which is what the two callees' record arms expect; the null twin case (no
        // vertex-elements CSO bound) reaches the same BindBackendVAOId(0) below that a null
        // frontend VAO always did.
        const SharedPtr<MG_State::GLState::VertexArrayObject> noFrontendVao;
        const auto& currentVAO = noFrontendVao;
        VertexArrayImpl::BackendVertexArrayObject* vaoTwin = nullptr;
        Uint32 vaoConfigVersion = 0;
        vaoTwin = VertexArrayImpl::ResolveVaoTwin(MG_Pipe::MGPipeApplier().BoundVertexElements);
        if (vaoTwin != nullptr) vaoTwin->AdoptUnchangedBufferSets(MG_Pipe::MGPipeApplier());
        // One program resolve and one texture-key capture serve the whole draw, for
        // the same reason the twin resolve does: only frontend GL entry points move
        // either, and none can run inside this preparation. GetProgramForDraw is a
        // cross-TU call with a guarded static inside - repeating it per stage showed
        // up in draw-loop profiles.
        //
        // P5e (pa), kimi row 91 / S4 1.1: AND ON THE RECORD ARM IT IS NOT MADE AT ALL. The row
        // this package retires is this single call - it is what MGP_INPUT_CHECK trips on, once
        // per draw, and it is 61 of the strict lane's 109 red entries. The arm is
        // DrawProgramFromRecords(), which names every consumer of the value rather than this
        // family alone; `currentProgram` stays null on it and the four callees below take their
        // own handle arms, none of which consults it. The null twin case (no program bound)
        // reaches the same "nothing to bind" answers a null frontend program always did.
        const SharedPtr<MG_State::GLState::ProgramObject> noFrontendProgram;
        const auto& currentProgram = noFrontendProgram;
        if (MG_Util::PipeStats::Enabled()) {
            // THE per-draw denominator for Espryt, plus this function's own two accessor
            // calls (the VAO and the draw program). Everything the callees below read is
            // counted by the callees that are instrumented; the rest is not counted (see
            // the inventory in PipeStats.cpp).
            MG_Util::PipeStats::AddCalls(MG_Util::PipeStats::CallClass::Draws, 1);
            // ZERO on the record arm, one per row still pulled otherwise: P5e (vi) retired the
            // bound-VAO accessor call and P5e (pa) the draw-program one, and the ledger has to
            // say so or the phase's own "accessor calls per draw" number would keep counting
            // reads that no longer happen. Two independent bits, so it is a sum and not a
            // three-way pick.
            MG_Util::PipeStats::AddCalls(MG_Util::PipeStats::CallClass::AccessorCalls, (0) + (0));
        }
        const TextureImpl::DrawTextureSyncKeys textureKeys = TextureImpl::CaptureDrawTextureSyncKeys();

        BufferImpl::SyncNeccessaryBuffers(currentVAO, vaoTwin, vaoConfigVersion, syncBit & DrawSyncBit::IndexBuffer,
                                          syncBit & DrawSyncBit::IndirectBuffer);
        VertexArrayImpl::SyncCurrentVAOFromRecords(vaoTwin);
        TextureImpl::SyncNeccessaryTextures(textureKeys);
        // A draw reads and writes through its image units too, so the unit bindings have to be
        // as current as the sampled ones. Gated (see the sweep): a program with no image binding
        // pays one integer test, and one with images re-issues them only when a texture shape
        // moved under them.
        TextureImpl::SyncImageTextureBindingsForDraw(textureKeys);
        // A draw writes through its image units too - the conformance case that found this
        // stores into a buffer texture from the FRAGMENT stage, not from a dispatch.
        TextureImpl::MarkWritableImageBufferTexturesGpuWritten();
        FramebufferImpl::SyncCurrentFBO();
        // P5e (pg), kimi row 91 / S4 1.1: the SharedPtr pull above is the row this family
        // retires. On the handle arm the program is MGPipeApplier().DrawProgram - a handle the
        // set_draw_program record carried - and `currentProgram` is not consulted for the sync
        // at all; the remaining uses of it below are the two callees' own, and both have a
        // handle arm of their own. P5e (pa) finished it: the pull itself is gone on that arm and
        // `currentProgram` is a null SharedPtr there, which this branch never reaches for.
        PrgramImpl::SyncCurrentProgramByHandle(MG_Pipe::MGPipeApplier().DrawProgram, true);
        RenderStateImpl::SyncRenderState();

        BindCurrentFBO(FramebufferTarget::Draw);

        {
#ifdef TRACY_ENABLE
            ZoneScopedNC("BindCurrentVAO", TRACY_ZONECOLOR_BACKEND);
#endif
            if (vaoTwin) {
                vaoTwin->Bind();
            } else {
                VertexArrayImpl::BindBackendVAOId(0);
            }
        }

        // P5e (pa): the third consumer, on ITS OWN arm and not on the hoist's. The hoist above is
        // a conjunction over four callees; this one is served by the record whenever the PROGRAM
        // family is on the handle arm, which includes the mixed A/B where the texture bits are
        // off and `currentProgram` above is therefore still a real object.
        VertexArrayImpl::SyncCurrentVertexAttributeValues(vaoTwin, MG_Pipe::MGPipeApplier().DrawProgram);

        BindCurrentTextures(textureKeys, currentProgram);
        BindCurrentProgramWithResources(currentProgram, textureKeys, MG_Pipe::MGPipeApplier().DrawProgram);

        // Last: opening the capture span needs the program current and the capture
        // buffers bound, and ES rejects most binding changes once it is open.
        XfbImpl::StartPendingTransformFeedback();
    }

    // Resolves every frontend texture unit's textures onto the backend context. Returns
    // false when the resolution could not be completed from the state it read - a bound
    // texture that has no backend object yet is skipped, and a later draw would bind it
    // without any of the memo keys below moving - so the caller must not memoise it.
    //
    // P5e (pa): `currentProgram` IS THE LEGACY WALK'S PARAMETER and is null under
    // TextureImpl::UnitTexturesByHandle(), where the by-handle pass below returns before the
    // only reader of it (sampledTargetForUnit) is even declared. The caller drops the object
    // only once that same test holds, so this is the arm speaking and not a pointer test.
    Bool ResolveAndBindUnitTextures(const SharedPtr<MG_State::GLState::ProgramObject>& currentProgram,
                                    Int maxTouchedUnit) {
#ifdef TRACY_ENABLE
        ZoneScopedNC("ResolveAndBindUnitTextures", TRACY_ZONECOLOR_BACKEND);
#endif
        Bool fullyResolved = true;
        // P5e (tx2), CONTRACT-P5E §5.2. ONE PASS OVER THE SAMPLER-VIEW WINDOW, AND THEN AN
        // UNBIND OF EVERY TARGET THE WINDOW DID NOT CLAIM.
        //
        // The three-pass slot walk below exists to arbitrate: desktop 1D/1D-array targets alias
        // ES 2D/2D-array, a unit can hold a real texture on one of an aliased pair and a default
        // object on the other, and two REAL textures can want one native target - which the
        // program's sampler types decide. THE CLIENT HAS ALREADY DECIDED ALL OF IT
        // (SamplerEmit.h's per-unit resolution is keyed on exactly those sampler types), so the
        // arbiter dies here rather than being re-implemented over records: one texture per unit,
        // named by the record, with its frontend target in the view CSO.
        //
        // The unbind half keeps its shape and changes its driver: instead of walking the frontend
        // unit's binding slots it walks the fixed TextureTarget enum against g_boundTexturesCache,
        // which is SERVER memory and is the shadow every redundant-bind filter in this backend
        // already trusts. That is what makes "the window did not claim it" mean "unbind it" - the
        // same statement the pre-handle pass makes with `!boundBackendTargets[...]`.
        const auto& st = MG_Pipe::MGPipeApplier();
        const Uint32 windowEnd = st.SamplerViewStart + st.SamplerViewCount;
        for (Int unit = 0; unit <= maxTouchedUnit; ++unit) {
            const Uint32 index = static_cast<Uint32>(unit);
            Array<Bool, (SizeT)TextureTarget::TextureTargetCount> boundBackendTargets{};
            if (index >= st.SamplerViewStart && index < windowEnd && index < st.BoundSamplerViews.size()) {
                const auto& view = st.BoundSamplerViews[index];
                if (!MG_Pipe::MGPipeHandleIsNull(view.Texture)) {
                    // The TARGET is the view CSO's, which is the frontend target the client
                    // resolved this unit against - not a second derivation from the texture's
                    // descriptor, which would be a second authority for one value.
                    const auto* viewRecord = PipeSamplerViewRecordForHandle(view.View);
                    const auto target = viewRecord != nullptr ? static_cast<TextureTarget>(viewRecord->View.Target)
                                                              : TextureTarget::Unknown;
                    if (viewRecord == nullptr) {
                        MGLOG_E_ONCE("MGPipe: unit %d's sampler view {%u, %u} has no applier record, "
                                     "so the target to bind texture {%u, %u} at is unknown; the unit "
                                     "keeps what it holds",
                                     static_cast<int>(unit), view.View.Slot, view.View.Gen, view.Texture.Slot,
                                     view.Texture.Gen);
                        fullyResolved = false;
                    } else if (!TextureImpl::IsSupportedTextureTarget(target)) {
                        MGLOG_D("    Texture target %s is not supported, skipping.",
                                MG_Util::ConvertTextureTargetToString(target).c_str());
                    } else if (auto* twin = TextureImpl::ResolveTextureTwin(view.Texture)) {
                        const GLenum targetGL = TextureImpl::ConvertTextureTargetToBackendGLEnum(target);
                        twin->Bind(targetGL, unit);
                        boundBackendTargets[static_cast<SizeT>(TextureImpl::MapToBackendTextureTarget(target))] = true;
                    } else {
                        // No twin yet: the sync pass that would have built it declined and
                        // named why. Not memoisable - a later draw would bind it without any
                        // key moving.
                        fullyResolved = false;
                    }
                }
            }
            // The backend half of glBindTexture(..., 0) for every native target this unit no
            // longer claims, driven by the server's own binding shadow.
            Array<Bool, (SizeT)TextureTarget::TextureTargetCount> visitedBackendTargets{};
            for (SizeT t = 0; t < (SizeT)TextureTarget::TextureTargetCount; ++t) {
                const auto target = static_cast<TextureTarget>(t);
                if (!TextureImpl::IsSupportedTextureTarget(target)) continue;
                const auto backendTargetIndex = static_cast<SizeT>(TextureImpl::MapToBackendTextureTarget(target));
                if (visitedBackendTargets[backendTargetIndex]) continue;
                visitedBackendTargets[backendTargetIndex] = true;
                if (boundBackendTargets[backendTargetIndex]) continue;
                if (TextureImpl::g_boundTexturesCache[static_cast<SizeT>(unit)][backendTargetIndex] == nullptr) {
                    continue;
                }
                TextureImpl::UnbindTexture(unit, TextureImpl::ConvertTextureTargetToBackendGLEnum(target));
            }
        }
        return fullyResolved;
    }

    // Per-unit memo of the sampler-registry lookup, NOT of the resulting unit binding
    // (BackendSamplerObject::Bind already dedups against g_boundSamplersCache). The registry
    // pairs a live frontend sampler with one backend object for the frontend's whole
    // lifetime: GetOrCreate replaces a pairing only when the frontend expired, entries are
    // erased only when the frontend expired, and a unit's sampler cannot expire while the
    // unit holds it - so owner-equality of the unit's current sampler against the snapshot
    // proves the cached pointer is exactly what Find would return. A miss result is never
    // cached: the backend object may not exist yet when the unit pass runs (the program
    // pass creates it later in the same draw), and a cached miss would keep skipping the
    // bind after it appears.
    struct UnitSamplerLookupMemo {
        // The {slot, gen} of the frontend sampler this row was resolved for. It replaces the
        // weak_ptr and its owner compare: a stale row cannot match, because the successor of a
        // freed sampler is handed the same slot only with a higher Gen.
        MG_Pipe::MGPipeHandle frontendHandle = MG_Pipe::kMGPipeNullHandle;
        SamplerImpl::BackendSamplerObject* backend = nullptr;
    };
    static Array<UnitSamplerLookupMemo, MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS>
        g_unitSamplerLookupMemos;

    // Puts each touched unit's frontend sampler object on the backend unit. Not part
    // of the texture memo below (BindCurrentProgramWithResources may rewrite sampled
    // units' samplers right after this runs - raw-depth-fetch substitution,
    // per-program sampler objects); instead the walk carries its own memo:
    //
    // The walk is a pure function of WHICH sampler object each touched unit holds -
    // covered by (context id, unit-bindings epoch, high-water mark; the epoch
    // snapshot includes each unit's sampler object and bumps when the mark moves) -
    // and replaying it as "do nothing" additionally requires the sampler bindings it
    // left to still be on the driver, which the row compare against
    // g_boundSamplersCache (the shadow every sampler bind routes through) proves.
    // Any later writer - the program pass substituting a raw-depth sampler, a
    // frontend sampler bind/unbind (epoch), the ES-context generation moving - lands
    // in one of those inputs. A unit whose sampler had no backend object yet leaves
    // its row untouched; the object is only created by the program pass, whose Bind
    // moves the row and thereby re-opens this memo, so the miss cannot be latched.
    Uint64 g_unitSamplerWalkContextId = 0;
    Uint64 g_unitSamplerWalkEpoch = 0;
    Int g_unitSamplerWalkMaxUnit = -1;
    Uint g_unitSamplerWalkContextGeneration = 0;
    Bool g_unitSamplerWalkValid = false;
    static decltype(SamplerImpl::g_boundSamplersCache) g_unitSamplerWalkRows{};

    void BindCurrentUnitSamplers(const TextureImpl::DrawTextureSyncKeys& keys) {
        const Int maxTouchedUnit = keys.maxTouchedUnit;
        const SizeT rowBytes = static_cast<SizeT>(maxTouchedUnit + 1) * sizeof(SamplerImpl::g_boundSamplersCache[0]);
        if (g_unitSamplerWalkValid && g_unitSamplerWalkContextId == keys.contextId &&
            g_unitSamplerWalkEpoch == keys.unitBindingsEpoch && g_unitSamplerWalkMaxUnit == maxTouchedUnit &&
            g_unitSamplerWalkContextGeneration == g_backendContextGeneration &&
            std::memcmp(g_unitSamplerWalkRows.data(), SamplerImpl::g_boundSamplersCache.data(), rowBytes) == 0) {
            return;
        }
        g_unitSamplerWalkValid = false;
        Bool walkedFromRecords = false;
        // P4a e4 (D-G2). THE WALK ITSELF READS THE APPLIER'S PER-UNIT HANDLE ARRAY.
        //
        // bind_sampler_states is the resolved answer to exactly the question this loop asks -
        // which sampler CSO does each touched unit carry - so the handle IS the lookup: no
        // GetTextureUnitObject per unit, no lifetime-id probe, and no UnitSamplerLookupMemo
        // row to keep, because FindByHandle is already one array index. The memo below stays
        // for the arm that still resolves through a frontend object.
        //
        // A NULL HANDLE MEANS "this unit has no sampler object", which is the record's way of
        // saying what the frontend's null SharedPtr says, and it unbinds - the texture's own
        // built-in sampler then applies, exactly as today. A live handle resolves to THE CSO's
        // OWN TWIN (P4a fable seam F-4, SamplerImpl::ResolveSamplerCsoTwin): the sentence that
        // stood here - "a handle whose twin does not exist yet is left alone ... the program
        // pass creates the twin later in the same draw" - described a lookup that could never
        // hit, because the handle is content-addressed and the registry's twins were minted off
        // lifetime ids, so this arm bound nothing on every draw and only the pre-handle program
        // pass ever put a glBindSampler'd object on the driver. The program pass now binds the
        // same CSO twin for the units it samples, which is what stops the two arms ping-ponging
        // a unit between two driver samplers.
        //
        // Declines - falls through to the frontend walk - until the set has arrived, and the
        // COUNT says so rather than the serial (MGPipeApplierReset advances serials whether or
        // not anything was emitted).
        // P4a decline-site S1: M - the mask says this family is not switched on, and it IS
        //   D's SamplerSubsystemEnabled() now. Silent, confirmed at the verification round.
        const auto& st = MG_Pipe::MGPipeApplier();
        // P4a decline-site S2: S - FLIPPED AT THE VERIFICATION ROUND to loud-once, then
        //   the frontend walk. Bit 11 is on (the enclosing SamplerSubsystemEnabled()) and
        //   package C has landed, so a draw that touches texture units with no
        //   bind_sampler_states ever applied is a seam defect. The frontend walk still
        //   runs: it answers the same question correctly, and what this line exists to stop
        //   is the SILENT permanent fallback, not the fallback.
        //   NARROWED BY THE FIRST REAL-PATH RUN, exactly as T2 was and for the same
        //   reason: maxTouchedUnit < 0 is a draw that touches no texture unit, for which an
        //   empty window is the correct emission. All 161 hits of the unnarrowed line were
        //   that shape - the log said `units 0..-1` on every one of them.
        if (st.SamplerStateCount == 0 && maxTouchedUnit >= 0) {
            // P5e (tx2), CONTRACT-P5E §5.3: the same refusal CurrentUnitBindingsEpoch makes -
            // the frontend walk below reads GetTextureUnitObject per unit, which an active
            // transport may not do for a record whose client has moved on.
            MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"BindSamplerStates.Count\"} - a draw "
                    "touches units 0..%d and no bind_sampler_states has ever been applied "
                    "while the sampler subsystem bit is set; the pre-handle sampler walk "
                    "reads client memory and is refused under an active transport "
                    "(CONTRACT-P5E.md §5.3)",
                    static_cast<int>(maxTouchedUnit));
            std::abort();
            MGLOG_E_ONCE("A sampler record does not describe the binding it names: a draw touches "
                         "units 0..%d and no bind_sampler_states has ever been applied while the "
                         "sampler subsystem bit is set; running the pre-handle sampler walk.",
                         static_cast<int>(maxTouchedUnit));
        }
        if (st.SamplerStateCount != 0) {
            for (Int unit = 0; unit <= maxTouchedUnit; ++unit) {
                const Uint32 index = static_cast<Uint32>(unit);
                // P4a decline-site S3: the OUT-OF-WINDOW UNBIND (MAJOR-2, fixed here); no
                //   flip remains at the verification round, only the A8 measurement below.
                //
                // A UNIT THE RECORD DOES NOT DESCRIBE IS UNBOUND, NOT SKIPPED. The var-tail
                // window IS the bound for RECORD RETENTION - entries outside it are not
                // cleared in the applier - but this loop is not reading records, it is
                // writing DRIVER state, and for a driver binding "no record" means "no
                // sampler object", which is exactly what the pre-handle arm's
                // `else { UnbindSampler(unit); }` below says and why it was written: a
                // sampler object left on the unit by an earlier draw keeps being applied,
                // and on a multisample texture - which takes no sampler object at all - the
                // draw is rejected outright. `continue` here reintroduced that defect on
                // the new arm for every unit below SamplerStateStart or above the window,
                // which is the same smaller-window direction DEV-3 refused.
                //
                // A8 again: nothing in the contract pins the window to cover
                // [0, maxTouchedUnit]. If package C documents that it does, this branch
                // becomes unreachable and can be asserted rather than executed; until then
                // it is load-bearing, and the verification round measures
                // (SamplerStateStart, SamplerStateCount) against maxTouchedUnit to say so.
                if (index < st.SamplerStateStart || index - st.SamplerStateStart >= st.SamplerStateCount) {
                    SamplerImpl::UnbindSampler(unit);
                    continue;
                }
                const MG_Pipe::MGPipeHandle sampler = st.BoundSamplerStates[index];
                if (MG_Pipe::MGPipeHandleIsNull(sampler)) {
                    SamplerImpl::UnbindSampler(unit);
                    continue;
                }
                // P4a decline-site S4, RETIRED at the fable seam round (F-4): what stood here
                //   was `g_backendSamplerObjects.FindByHandle(sampler)` - an identity-keyed
                //   table asked for a content-addressed handle, a miss by construction on
                //   every draw - with a comment that read the miss as a within-draw ordering
                //   fact. The twin is the CSO's own now; a null answer has already named its
                //   reason (no record, or a slot that cannot be adopted) and leaves the unit
                //   as it is, which is the one decline this arm still has.
                if (auto* twin = SamplerImpl::ResolveSamplerCsoTwin(sampler)) {
                    twin->Bind(unit);
                }
            }
            walkedFromRecords = true;
        }
        if (!walkedFromRecords) {
            // P13: no bind_sampler_states window applied, i.e. no unit holds a sampler object -
            // the record arm emits one for every unit that does. Unbind, so a sampler object
            // left on a unit by an earlier draw is not applied to this one.
            for (Int unit = 0; unit <= maxTouchedUnit; ++unit)
                SamplerImpl::UnbindSampler(unit);
        }
        g_unitSamplerWalkContextId = keys.contextId;
        g_unitSamplerWalkEpoch = keys.unitBindingsEpoch;
        g_unitSamplerWalkMaxUnit = maxTouchedUnit;
        g_unitSamplerWalkContextGeneration = g_backendContextGeneration;
        std::memcpy(g_unitSamplerWalkRows.data(), SamplerImpl::g_boundSamplersCache.data(), rowBytes);
        g_unitSamplerWalkValid = true;
    }
    // Small per-PROGRAM memo set, not one global: the program is part of the key
    // (it arbitrates aliased native targets), so a frame that cycles a handful of
    // programs over UNCHANGED unit bindings - Sodium issues 62 glUseProgram per
    // frame - invalidated a single memo on every switch and re-ran the full
    // three-pass slot walk for an answer that had not moved. Entries are selected
    // by raw program pointer; everything a recycled address could confuse is
    // caught by the full key compare (lifetime id) exactly as before. Round-robin
    // eviction; 4 entries cover the shader set a Minecraft-shaped frame cycles.
    Array<ResolvedTextureBindingMemo, 4> g_resolvedTextureBindingMemos;
    Uint32 g_resolvedTextureBindingMemoCursor = 0;

    // Rebinds every frontend texture unit's textures (and sampler objects) on the
    // backend context. Needed before draws AND compute dispatches: content syncs
    // (SyncTextureObjectToBackend) bind scratch textures on the active unit as a
    // side effect, so unit bindings must be re-established afterwards or shaders
    // sample whatever texture the last sync left behind (e.g. Flywheel's depth
    // pyramid downsample reading a stale unit-0 binding instead of the depth
    // attachment).
    //
    // P5e (pa): `currentProgram` IS NULL ON THE HANDLE ARM, and that is a statement about the
    // caller's arm rather than something this body may test for. Every read of it below sits on
    // the `byHandle == false` side of a branch whose condition is TextureImpl::UnitTexturesByHandle()
    // - the memo's entry selection, the four frontend key rows, and ResolveAndBindUnitTextures'
    // sampledTargetForUnit lambda, which its own handle arm returns before reaching. The hoist in
    // PrepareForDraw only drops the object once THAT test holds too (DrawProgramFromRecords), so
    // this parameter is null exactly where nothing consults it.
    void BindCurrentTextures(const TextureImpl::DrawTextureSyncKeys& keys,
                             const SharedPtr<MG_State::GLState::ProgramObject>& currentProgram) {
#ifdef TRACY_ENABLE
        ZoneScopedNC("BindCurrentTextures", TRACY_ZONECOLOR_BACKEND);
#endif
        // Units past the frontend's high-water mark have provably-empty slots.
        const Int maxTouchedUnit = keys.maxTouchedUnit;

        // P5e (tx2) / ruling 6: on the handle arm the key is the applier's rows and nothing
        // frontend - INCLUDING the entry selection, which used the frontend program's address.
        // maxTouchedUnit stays in it because the unbind sweep this memo replays is bounded by it.
        MG_Pipe::MGPipeHandle drawProgram = MG_Pipe::kMGPipeNullHandle;
        Uint64 shaderCsoSerial = 0;
        Uint64 bindingsSerial = 0;
        Uint64 samplerViewsSerial = 0;
        Uint64 contextSerial = 0;
        const auto& st = MG_Pipe::MGPipeApplier();
        drawProgram = st.DrawProgram;
        samplerViewsSerial = st.SamplerViewsSerial;
        contextSerial = st.ContextSerial;
        if (const auto* cso = PipeShaderCsoRecordForHandle(drawProgram)) {
            shaderCsoSerial = cso->Serial;
            bindingsSerial = cso->BindingsSerial;
        }
        // Entry selection by program pointer; a missing program takes the round-robin
        // victim. WHICH entry is used is only a performance choice - correctness sits
        // entirely in the full key + shadow compare below, unchanged from the single
        // memo this set replaces.
        //
        // P5e (pa): NOT EVEN THE POINTER, on the handle arm. The by-handle branch below selects
        // and keys on {DrawProgram, ShaderCso.Serial, BindingsSerial} (ruling ID-86) and never
        // looks at this value, but it was still being loaded out of the caller's SharedPtr every
        // draw - and once the caller stops holding one there is nothing there to load.
        const void* programKey = nullptr;
        ResolvedTextureBindingMemo* memoSlot = nullptr;
        for (auto& candidate : g_resolvedTextureBindingMemos) {
            if (candidate.valid && candidate.drawProgram.Slot == drawProgram.Slot &&
                candidate.drawProgram.Gen == drawProgram.Gen) {
                memoSlot = &candidate;
                break;
            }
            continue;
        }
        if (!memoSlot) {
            g_resolvedTextureBindingMemoCursor =
                (g_resolvedTextureBindingMemoCursor + 1) % g_resolvedTextureBindingMemos.size();
            memoSlot = &g_resolvedTextureBindingMemos[g_resolvedTextureBindingMemoCursor];
        }
        auto& memo = *memoSlot;
        const SizeT shadowBytes = static_cast<SizeT>(maxTouchedUnit + 1) * sizeof(TextureImpl::g_boundTexturesCache[0]);
        const Uint64 unitBindingsEpoch = keys.unitBindingsEpoch;
        // P5e (ID-124): the legacy key is a LAZY LAMBDA, not an eagerly-computed Bool. It was
        // the else-arm of a ternary whose declaration sat inside `#if MOBILEGL_BUILD_DISAGGREGATED`
        // while the arm itself and every use of `keysMatch` sat outside it, so neither
        // non-disaggregated flavour compiled. Hoisting it to an eager Bool would have been
        // WORSE than the build break: it reads `currentProgram`, which is null on the handle
        // arm, so the laziness the ternary gave for free is load-bearing (ID-81 / ID-110 -
        // the arm decides, and the other arm's reads must not happen at all).
        const auto legacyKeysMatch = [&]() -> Bool {
            return (memo.valid && memo.glContextId == keys.contextId && memo.maxTouchedUnit == maxTouchedUnit &&
                    memo.unitBindingsEpoch == unitBindingsEpoch &&
                    memo.samplingResolutionGeneration == keys.samplingGeneration && memo.program == programKey &&
                    memo.programLifetimeId == (currentProgram ? currentProgram->GetLifetimeId() : 0) &&
                    memo.programBackendStateVersion ==
                        (currentProgram ? currentProgram->GetBackendStateVersion() : 0) &&
                    memo.programLinked == (currentProgram && currentProgram->GetLinkStatus()) &&
                    memo.contextGeneration == g_backendContextGeneration);
        };
        const Bool keysMatch =
            (memo.valid && memo.maxTouchedUnit == maxTouchedUnit && memo.contextSerial == contextSerial &&
             memo.samplerViewsSerial == samplerViewsSerial && memo.drawProgram.Slot == drawProgram.Slot &&
             memo.drawProgram.Gen == drawProgram.Gen && memo.shaderCsoSerial == shaderCsoSerial &&
             memo.bindingsSerial == bindingsSerial && memo.contextGeneration == g_backendContextGeneration);
        // Short-circuited: the shadow compare is only meaningful once the key (and with it the
        // snapshotted row count) matches.
        if (!keysMatch ||
            std::memcmp(memo.boundTextures.data(), TextureImpl::g_boundTexturesCache.data(), shadowBytes) != 0) {
            memo.valid = false;
            if (ResolveAndBindUnitTextures(currentProgram, maxTouchedUnit)) {
                memo.glContextId = keys.contextId;
                memo.maxTouchedUnit = maxTouchedUnit;
                memo.unitBindingsEpoch = unitBindingsEpoch;
                memo.samplingResolutionGeneration = keys.samplingGeneration;
                memo.program = programKey;
                // P5e (pa): the four frontend program rows belong to the LEGACY key and are not
                // written on the handle arm - decided by `byHandle`, which is the arm, and not by
                // the object happening to be null, which is only a consequence of it. They are
                // zeroed rather than skipped so an entry cannot carry a row nobody wrote.
                memo.programLifetimeId = 0;
                memo.programBackendStateVersion = 0;
                memo.programLinked = false;
                memo.contextGeneration = g_backendContextGeneration;
                memo.drawProgram = drawProgram;
                memo.shaderCsoSerial = shaderCsoSerial;
                memo.bindingsSerial = bindingsSerial;
                memo.samplerViewsSerial = samplerViewsSerial;
                memo.contextSerial = contextSerial;
                std::memcpy(memo.boundTextures.data(), TextureImpl::g_boundTexturesCache.data(), shadowBytes);
                memo.valid = true;
            }
        }

        BindCurrentUnitSamplers(keys);
    }

    void BindCurrentTextures() {
        // P5e (pa): the exported no-argument entry (DirectGLES.h) takes the SAME arm the hoist in
        // PrepareForDraw does, and for the same reason: the pull is what trips the input check, so
        // an entry point that keeps it would keep the row alive for whichever caller reaches this
        // overload next. Today that is SanityTest's unit cases, which run under monolith
        // transport and therefore take the frontend arm below unchanged.
        const SharedPtr<MG_State::GLState::ProgramObject> noFrontendProgram;
        BindCurrentTextures(TextureImpl::CaptureDrawTextureSyncKeys(), noFrontendProgram);
    }

    // Binds the current program's backend object and re-establishes its per-program
    // resources: global UBO contents, uniform-block bindings, and sampler uniform
    // units (layout(binding=N) qualifiers are stripped from transpiled ESSL, so the
    // association must be rebuilt through the API). Compute dispatches depend on
    // this as much as draws do — e.g. Flywheel's cull shader reads the
    // _FlwFrameUniforms block and the _flw_depthPyramid sampler.
    //
    // P5e (pa): `currentProgram` IS NULL WHENEVER `handleArm` HOLDS. Every read of it in the
    // body below is on the `:` side of a `handleArm ? <record> :` pick or inside the `else` of
    // `if (handleArm)`, with one pair that is guarded instead - the two `globalConstants ? ... :`
    // reads, which the [[noreturn]] refusal a few lines above them makes unreachable on that arm
    // (its note says so). The parameter stays for the monolith arm, which is ruling ID-81: that
    // build keeps this function's frontend text token for token.
    void BindCurrentProgramWithResources(const SharedPtr<MG_State::GLState::ProgramObject>& currentProgram,
                                         const TextureImpl::DrawTextureSyncKeys& keys,
                                         MG_Pipe::MGPipeHandle programCso) {
        // P5e (pg): THE HANDLE ARM'S GATE, from the record rather than from the object. Hoisted
        // out of the `if` below because the whole body's entry condition is "the program linked
        // and has SPIR-V", and on this arm both answers are fields of the descriptor
        // (Desc.LinkStatus is ID-88's, Desc.SpirvStatus P4a's).
        const MG_Pipe::MGPipeShaderCsoRecord* programRecord = nullptr;
        programRecord = PrgramImpl::FindShaderCsoRecord(programCso);
        if (programRecord != nullptr && (programRecord->Desc.LinkStatus == 0 || programRecord->Desc.SpirvStatus == 0)) {
            programRecord = nullptr;
        }
        const Bool programUsable = programRecord != nullptr;
        if (programUsable) {
#ifdef TRACY_ENABLE
            ZoneScopedNC("BindCurrentProgram", TRACY_ZONECOLOR_BACKEND);
#endif
            PrgramImpl::BackendProgramObjectImpl* twin = nullptr;
            // THE STASH, KEYED ON THE HANDLE (CONTRACT-P5E §5.5). SyncCurrentProgramByHandle
            // wrote it a few lines ago in the same Prepare; the fallback is the by-handle
            // resolver, which is record-first and touches no frontend identity - so unlike
            // the frontend fallback below it is not a registry probe at all.
            twin = PrgramImpl::g_currentDrawProgramHandle == programCso ? PrgramImpl::g_currentDrawBackendProgram
                                                                        : nullptr;
            if (!twin) twin = PrgramImpl::ResolveProgramTwin(programCso);
            if (twin) {
                auto& backendProgram = *twin;
                backendProgram.Use();

                // Global UBO: block index and binding-point assignment are cached at
                // link time (CacheResourceLocations); re-upload only when the CPU shadow
                // actually changed since the last upload for this program.
                // P5e (pg): the block's SIZE is Desc.GlobalUboSize on the handle arm. It is the
                // same number GetUBOSize() answers - the emitter takes it from there - but it is
                // the record's copy, which is the point.
                const Uint globalUboSize = programRecord->Desc.GlobalUboSize;
                if (globalUboSize > 0 && backendProgram.HasGlobalUboBlock()) {
#ifdef TRACY_ENABLE
                    ZoneScopedNC("UpdateGlobalUBO", TRACY_ZONECOLOR_BACKEND);
#endif
                    // P4a e4 (D-H6): set_global_constants' counterpart. The version and the
                    // block image come off the ShaderCso record when it carries them; the UBO
                    // RING BELOW IS UNTOUCHED - it is on the Espryt do-not-touch list, keeps
                    // its {contentVersion, ringGeneration, frameSerial, offset} allocation
                    // key, keeps its glBufferSubData fallback and keeps accounting to
                    // ByteClass::StageUboGlobal. What changes is where the version and the
                    // bytes come from, and nothing else.
                    //
                    // The record covers the DEFAULT UNIFORM BLOCK ONLY. Named UBOs are the
                    // block right after this one and they are P4b's (dirty bits 15-17), which
                    // is why nothing here touches them.
                    const MG_Pipe::MGPipeShaderCsoRecord* const globalConstants =
                        PrgramImpl::ResolveGlobalConstantsRecordForHandle(programCso, globalUboSize);
                    // P5e (pg), gap G-C: `MapUBO()` HAS NO RUN-AHEAD ANSWER. It returns
                    // ProgramObject::globalUboScratch - the live CPU array every glUniform*
                    // writes into - so under a transport it is a torn read of client memory by
                    // construction, not a read that happens to race. Every reason
                    // ResolveGlobalConstantsRecord* can decline is a SEAM DEFECT here (bit 12
                    // off, the mis-keyed descriptor, the never-uploaded sentinel, a short block
                    // image), and each of them has already said so by name one level down; what
                    // this adds is that on the handle arm there is nothing legal to fall back
                    // to. CONTRACT-P5E §5.5: Fatal{UnmigratedVerb, "set_global_constants"}, not
                    // a torn upload.
                    //
                    // P5e (pa): AND IT IS ALSO WHAT KEEPS THE TWO `globalConstants ? ... :
                    // currentProgram->...` READS BELOW OFF THE FRONTEND. std::abort() is
                    // [[noreturn]], so past this statement `handleArm` implies
                    // `globalConstants != nullptr` in one step and in this function - not by a
                    // chain through other files, which is the distinction ID-110 draws. It
                    // matters now: pa's caller passes a NULL ProgramObject on that arm, so the
                    // `:` sides are unreachable rather than merely unused.
                    if (globalConstants == nullptr) {
                        MGLOG_F("MGPipe: Fatal{UnmigratedVerb, \"set_global_constants\"} - the "
                                "ShaderCso record {%u, %u} does not answer the default uniform "
                                "block for a draw of a program that declares %u bytes of it, and "
                                "MapUBO() is the client's live scratch array",
                                programCso.Slot, programCso.Gen, globalUboSize);
                        std::abort();
                    }
                    const Uint32 uboContentVersion = globalConstants ? globalConstants->GlobalConstantsVersion
                                                                     : currentProgram->GetUBOContentVersion();
                    const SizeT uboSize = static_cast<SizeT>(globalUboSize);
                    // Substituted at the two upload sites rather than hoisted into a local:
                    // MapUBO() is called there today and only there, and hoisting it would
                    // call it on paths that skip the upload entirely.
#define MGB_UBO_BYTES                                                                                                  \
    (globalConstants ? static_cast<const void*>(globalConstants->GlobalConstants.data())                               \
                     : static_cast<const void*>(currentProgram->MapUBO()))
                    // Preferred path: write changed contents into a fresh slot of the
                    // shared persistent-mapped ring and bind it as a range. The GPU
                    // never reads bytes the CPU is writing, so the driver has no
                    // write-after-read hazard to resolve — the in-place glBufferSubData
                    // below forced Adreno into a ghost/stall on every uniform-dirtying
                    // draw (MC dirties uniforms every draw), which dominated frame time.
                    Bool ringBound = false;
                    {
                        const SizeT bindSize =
                            std::max(uboSize, static_cast<SizeT>(backendProgram.GetGlobalUboBackendBlockSize()));
                        auto& ringSlot = backendProgram.GetGlobalUboRingAllocation();
                        void* write = nullptr;
                        Uint ringId = 0;
                        const auto state = BufferImpl::UboRingPrepareSlot(
                            ringSlot, uboContentVersion, CurrentFrameSerial(), bindSize, write, ringId);
                        if (state == BufferImpl::UboRingSlotState::Allocated) {
                            std::memcpy(write, MGB_UBO_BYTES, uboSize);
                            if (MG_Util::PipeStats::Enabled()) {
                                MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::StageUboGlobal,
                                                             static_cast<Uint64>(uboSize));
                            }
                        }
                        if (state == BufferImpl::UboRingSlotState::Allocated ||
                            state == BufferImpl::UboRingSlotState::Reused) {
                            BufferImpl::BindBufferRangeCached(GL_UNIFORM_BUFFER, 0, ringId,
                                                              static_cast<GLintptr>(ringSlot.offset),
                                                              static_cast<GLsizeiptr>(bindSize));
                            ringBound = true;
                        }
                    }
                    if (!ringBound) {
                        // Fallback (no EXT_buffer_storage / fences, or ring creation
                        // failed): the original in-place upload.
                        if (backendProgram.GetLastUploadedGlobalUboVersion() != uboContentVersion) {
                            g_GLESFuncs.glBindBuffer(GL_UNIFORM_BUFFER, backendProgram.GetBackendGlobalUBOId());
                            g_GLESFuncs.glBufferSubData(GL_UNIFORM_BUFFER, 0, globalUboSize, MGB_UBO_BYTES);
                            if (MG_Util::PipeStats::Enabled()) {
                                MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::StageUboGlobal,
                                                             static_cast<Uint64>(globalUboSize));
                            }
                            g_GLESFuncs.glBindBuffer(GL_UNIFORM_BUFFER, 0);
                            backendProgram.SetLastUploadedGlobalUboVersion(uboContentVersion);
                        }
                        BufferImpl::BindBufferBaseCached(GL_UNIFORM_BUFFER, 0, backendProgram.GetBackendGlobalUBOId());
                    }
#undef MGB_UBO_BYTES
                }

                {
#ifdef TRACY_ENABLE
                    ZoneScopedNC("UpdateUBO", TRACY_ZONECOLOR_BACKEND);
#endif
                    // Normal UBOs: backend block indices and glUniformBlockBinding
                    // assignments are cached at link time; per draw only the buffer
                    // bindings are re-established (they follow frontend binding points).
                    const auto& blockBackendIndices = backendProgram.GetUniformBlockBackendIndices();
                    const auto uboCount = static_cast<Int>(blockBackendIndices.size());
                    // Pre-loop epoch read; the per-resource drawCleanEpoch stamps below
                    // follow the same stamp-the-pre-read-value rule as the VAO memo
                    // (Managers.h documents the contract and the mutation-site list).
                    const Uint64 bufferEpoch = uboCount > 0 ? BufferImpl::CurrentBufferMutationEpoch() : 0;
                    Uint lastUBOBinding = 0; // binding 0 is reserved for the global UBO
                    for (Int i = 0; i < uboCount; ++i) {
                        ++lastUBOBinding;
                        if (blockBackendIndices[i] < 0) {
                            continue;
                        }

                        // Connect buffer to backend binding point.
                        //
                        // P5e (pg): glUniformBlockBinding MOVES THIS AFTER THE LINK, so it is
                        // one of the three post-link mutable reflection fields the archive alone
                        // cannot answer - set_program_bindings' first tail carries it, dense in
                        // this very index space. The BINDING POINT it names is sb's row (the
                        // GetBufferBindingPoint below); this line is the boundary S4 R5 named.
                        auto binding = PrgramImpl::ProgramBlockBindingFromRecord(*programRecord, i);
                        // P5e (sb, §5.6): THE POINT HALF, by record. `binding` above is the
                        // PROGRAM's block binding and stays exactly where it is (it is package
                        // pg's row); what moves is the point it indexes. The index space is the
                        // same one - Start is 0 by contract - so `binding` addresses the
                        // applier's window directly, and a binding at or above Count means
                        // "nothing bound", which is what the frontend array's default said too.
                        const MG_Pipe::MGPipeApplierState& st = MG_Pipe::MGPipeApplier();
                        const Uint32 windowStart = st.ShaderBufferStart[MG_Pipe::kMGPipeShaderBufferClassUniform];
                        const SizeT windowEnd = static_cast<SizeT>(windowStart) +
                                                st.ShaderBufferCount[MG_Pipe::kMGPipeShaderBufferClassUniform];
                        if (static_cast<SizeT>(binding) < windowStart || static_cast<SizeT>(binding) >= windowEnd) {
                            continue; // nothing bound there - the frontend arm's `if (bufferObj)`
                        }
                        const MG_Pipe::MGPBufferRange& entry =
                            st.BoundShaderBuffers[MG_Pipe::kMGPipeShaderBufferClassUniform]
                                                 [static_cast<SizeT>(binding)];
                        if (MG_Pipe::MGPipeHandleIsNull(entry.Res)) continue;
                        // The clean probe is the SAME five questions IsBufferDrawClean asks
                        // the frontend object, asked by handle: the twin's identity, the
                        // resource record's Serial against the twin's synced one, and the
                        // pending-work sets - all server-owned (D-A4). The drawCleanEpoch
                        // short-circuit is unchanged; it was never the point read.
                        auto* backendResource = BufferImpl::FindBufferResourceForHandle(entry.Res);
                        if (!backendResource || backendResource->drawCleanEpoch != bufferEpoch) {
                            if (backendResource &&
                                BufferImpl::IsBufferDrawCleanByHandle(entry.Res, backendResource, nullptr)) {
                                backendResource->drawCleanEpoch = bufferEpoch;
                            } else {
                                backendResource = BufferImpl::EnsureBufferResourceForHandle(nullptr, entry.Res);
                            }
                        }
                        if (backendResource && backendResource->id != 0) {
                            // WHOLE-VS-RANGE IS THE RECORD's TEST, not `range.end == 0`: a
                            // base binding travels as kMGPipeWholeBuffer precisely so the
                            // extent is re-resolved HERE, against the storage this server
                            // holds, rather than frozen at the client's emission.
                            if (entry.Offset == 0 && entry.Size == MG_Pipe::kMGPipeWholeBuffer) {
                                BufferImpl::BindBufferBaseCached(GL_UNIFORM_BUFFER, lastUBOBinding,
                                                                 backendResource->id);
                            } else {
                                const SizeT storage = backendResource->storageSize;
                                const SizeT rangeStart = std::min<SizeT>(static_cast<SizeT>(entry.Offset), storage);
                                const SizeT rangeEnd =
                                    std::min<SizeT>(static_cast<SizeT>(entry.Offset + entry.Size), storage);
                                BufferImpl::BindBufferRangeCached(
                                    GL_UNIFORM_BUFFER, lastUBOBinding, backendResource->id,
                                    static_cast<GLintptr>(rangeStart), static_cast<GLsizeiptr>(rangeEnd - rangeStart));
                            }
                        } else {
                            MGLOG_E_ONCE("No backend buffer found for UBO binding, cannot bind UBO.");
                        }
                        continue;
                    }
                }

                // Atomic counter buffers. Bound here rather than beside the storage-buffer sync
                // in SyncNeccessaryBuffers because the reserved slot the transpiled ESSL reads
                // them at is PROGRAM state: it is `top - GL binding` for the counter blocks THIS
                // program declares, and no other program's blocks live there. Both the draw and
                // the dispatch path reach this, which is what a compute-shader counter needs.
                if (!backendProgram.GetAtomicCounterBindings().empty()) {
                    BufferImpl::SyncAtomicCounterBuffers(backendProgram.GetAtomicCounterBindings(),
                                                         backendProgram.GetAtomicCounterEsslBindingTop());
                }

                {
#ifdef TRACY_ENABLE
                    ZoneScopedNC("BindSamplerUnit", TRACY_ZONECOLOR_BACKEND);
#endif
                    // Sampler unit binding: backend locations are cached at link time;
                    // glUniform1i is program state, so it is only re-issued when the
                    // frontend-assigned unit differs from what this program last saw.
                    //
                    // The whole pass sits behind the per-twin SamplerPassMemo (see its
                    // declaration for the invalidation enumeration): while the keys hold
                    // and every previously touched unit's sampler-shadow row is exactly
                    // what this pass last left there, re-running it is a provable no-op.
                    auto& samplerPassMemo = backendProgram.GetSamplerPassMemo();
                    // P5e (pg), CONTRACT-P5E §5.5: THE SAMPLER PASS MEMO KEYS ON BindingsSerial
                    // on the handle arm. m_backendStateVersion is a frontend counter read on
                    // every draw (kimi row 105); what it is standing in for here is "has a
                    // sampler uniform's unit moved", and that is exactly what an applied
                    // set_program_bindings means. Truncated to the memo's Uint32 field, which is
                    // safe for a memo key - it is a change detector, not an ordering - and the
                    // record's serial would need four billion binding records to wrap.
                    const Uint32 programBackendStateVersion = static_cast<Uint32>(programRecord->BindingsSerial);
                    Bool samplerPassClean = samplerPassMemo.valid && samplerPassMemo.contextId == keys.contextId &&
                                            samplerPassMemo.unitBindingsEpoch == keys.unitBindingsEpoch &&
                                            samplerPassMemo.samplingGeneration == keys.samplingGeneration &&
                                            samplerPassMemo.backendStateVersion == programBackendStateVersion &&
                                            samplerPassMemo.textureContextGeneration == g_backendContextGeneration;
                    if (samplerPassClean) {
                        for (Uint i = 0; i < samplerPassMemo.count; ++i) {
                            if (SamplerImpl::g_boundSamplersCache[samplerPassMemo.units[i]] !=
                                samplerPassMemo.rows[i]) {
                                samplerPassClean = false;
                                break;
                            }
                        }
                    }
                    if (!samplerPassClean) {
                        constexpr SizeT kMaxMemoEntries =
                            PrgramImpl::BackendProgramObjectImpl::SamplerPassMemo::kMaxEntries;
                        samplerPassMemo.valid = false;
                        samplerPassMemo.count = 0;
                        Bool memoisable = true;
                        for (auto& samplerBinding : backendProgram.GetSamplerUniformBindings()) {
                            // P5e (pg): the unit glUniform1i put on this sampler - the second
                            // of the three post-link mutable fields, carried sparse and ascending
                            // by location in set_program_bindings' second tail.
                            const auto unit = PrgramImpl::ProgramSamplerUnitFromRecord(*programRecord,
                                                                                       samplerBinding.frontendLocation);
                            if (unit == -1) continue;
                            // Record the touched unit for the memo; a pass touching more
                            // units than the memo can carry (or an out-of-range unit)
                            // simply never memoises.
                            if (memoisable && samplerPassMemo.count < kMaxMemoEntries && unit >= 0 &&
                                unit < static_cast<Int>(SamplerImpl::g_boundSamplersCache.size())) {
                                samplerPassMemo.units[samplerPassMemo.count++] = static_cast<Uint8>(unit);
                            } else {
                                memoisable = false;
                            }
                            if (samplerBinding.lastAssignedUnit != unit) {
                                g_GLESFuncs.glUniform1i(samplerBinding.backendLocation, unit);
                                samplerBinding.lastAssignedUnit = unit;
                            }

                            // P5e (tx2), CONTRACT-P5E §5.3. THE SAMPLER PASS, FROM RECORDS.
                            //
                            // Four frontend reads used to live in this loop body and every one of
                            // them has a carrier: the unit's sampler object is
                            // st.BoundSamplerStates[u] (null = the texture's built-in sampler, which
                            // is TextureResources[view.Texture].Params.BuiltinSampler); its fifteen
                            // values - lodBias among them - are the CSO record's
                            // (MGPipeValueTypes.h); and the sampled texture's FORMAT, which decides
                            // the raw-depth-fetch substitution, is
                            // SamplerViewCsos[View].View.InternalFormat.
                            //
                            // THE SUBSTITUTION ITSELF STAYS ON THE SERVER (ARCHITECTURE.md §5.5
                            // assigns it there by name); what moves is where its two inputs come
                            // from.
                            const auto& st = MG_Pipe::MGPipeApplier();
                            const auto index = static_cast<Uint32>(unit);
                            const Bool inViewWindow = index >= st.SamplerViewStart &&
                                                      index - st.SamplerViewStart < st.SamplerViewCount &&
                                                      index < st.BoundSamplerViews.size();
                            const MG_Pipe::MGPBoundView view =
                                inViewWindow ? st.BoundSamplerViews[index] : MG_Pipe::MGPBoundView{};
                            const Bool inStateWindow = index >= st.SamplerStateStart &&
                                                       index - st.SamplerStateStart < st.SamplerStateCount &&
                                                       index < st.BoundSamplerStates.size();
                            const MG_Pipe::MGPipeHandle unitSampler =
                                inStateWindow ? st.BoundSamplerStates[index] : MG_Pipe::kMGPipeNullHandle;

                            // The EFFECTIVE sampler of the unit: the bound CSO if there is one,
                            // else the sampled texture's built-in sampler CSO - exactly the
                            // override order GL states and the pre-handle body spells with
                            // `samplerObject ? ... : texture->GetSamplerObject()`.
                            MG_Pipe::MGPipeHandle effectiveCso = unitSampler;
                            if (MG_Pipe::MGPipeHandleIsNull(effectiveCso) &&
                                !MG_Pipe::MGPipeHandleIsNull(view.Texture)) {
                                if (const auto* texRecord = PipeTextureRecordForHandle(view.Texture)) {
                                    effectiveCso = texRecord->Params.BuiltinSampler;
                                }
                            }
                            const MG_Pipe::MGPipeSamplerCsoRecord* effectiveCsoRecord =
                                PipeSamplerCsoRecordForHandle(effectiveCso);

                            if (samplerBinding.lodBiasLocation >= 0) {
                                const Float lodBias =
                                    effectiveCsoRecord != nullptr ? effectiveCsoRecord->Params.lodBias : 0.0f;
                                if (lodBias != samplerBinding.lastAssignedLodBias) {
                                    g_GLESFuncs.glUniform1f(samplerBinding.lodBiasLocation, lodBias);
                                    samplerBinding.lastAssignedLodBias = lodBias;
                                }
                            }

                            // The last sample a usampler2DMS fetch may name, from the texture
                            // actually bound: a stencil texture's real count is not an integer
                            // colour texture's. Keyed like the bias above - a texture's storage or
                            // a unit's binding moving is what invalidates this pass's memo.
                            if (samplerBinding.msSampleMaxLocation >= 0) {
                                const Int sampleMax =
                                    std::max(PipeTextureBackendSampleCountForHandle(view.Texture), 1) - 1;
                                if (sampleMax != samplerBinding.lastAssignedMsSampleMax) {
                                    g_GLESFuncs.glUniform1i(samplerBinding.msSampleMaxLocation, sampleMax);
                                    samplerBinding.lastAssignedMsSampleMax = sampleMax;
                                }
                            }

                            const auto* viewRecord = PipeSamplerViewRecordForHandle(view.View);
                            const Bool sampledIsTexture2D =
                                viewRecord != nullptr &&
                                static_cast<TextureTarget>(viewRecord->View.Target) == TextureTarget::Texture2D;
                            if (samplerBinding.uniformType == GL_SAMPLER_2D && sampledIsTexture2D &&
                                effectiveCsoRecord != nullptr &&
                                NeedsRawDepthFetchSampler(
                                    effectiveCsoRecord->Params,
                                    static_cast<TextureInternalFormat>(viewRecord->View.InternalFormat))) {
                                GetRawDepthFetchSampler()->Bind(unit);
                                MGLOG_D("Using raw depth fetch sampler on unit %d.", unit);
                            } else if (!MG_Pipe::MGPipeHandleIsNull(unitSampler)) {
                                // THE MINT IS GONE (§4.2): the unit's sampler is the CSO's own
                                // twin, the same one BindCurrentUnitSamplers binds, so the two
                                // arms cannot hand a unit back and forth between two driver
                                // samplers - and there is no arm left that creates a twin from a
                                // frontend SamplerObject.
                                if (auto* csoTwin = SamplerImpl::ResolveSamplerCsoTwin(unitSampler)) {
                                    csoTwin->Bind(unit);
                                }
                            } else {
                                SamplerImpl::UnbindSampler(unit);
                            }
                            continue;
                        }
                        if (memoisable) {
                            // Snapshot the rows AFTER the pass: they are exactly what a
                            // clean replay must find untouched.
                            for (Uint i = 0; i < samplerPassMemo.count; ++i) {
                                samplerPassMemo.rows[i] = SamplerImpl::g_boundSamplersCache[samplerPassMemo.units[i]];
                            }
                            samplerPassMemo.contextId = keys.contextId;
                            samplerPassMemo.unitBindingsEpoch = keys.unitBindingsEpoch;
                            samplerPassMemo.samplingGeneration = keys.samplingGeneration;
                            samplerPassMemo.backendStateVersion = programBackendStateVersion;
                            samplerPassMemo.textureContextGeneration = g_backendContextGeneration;
                            samplerPassMemo.valid = true;
                        }
                    }
                }
            } else {
                g_GLESFuncs.glUseProgram(0);
                PrgramImpl::g_lastUsedBackendProgramId = 0;
                MGLOG_E_ONCE("No backend program found (maybe not synced) for current program, cannot use program.");
            }
        }
    }

    // Raw pointer: only ever used inside one GL entry point after PrepareForDraw/
    // PrepareForCompute, where the current program (and therefore its registry twin)
    // is pinned for the duration. Prefers the per-draw stash those preparations wrote.
    PrgramImpl::BackendProgramObjectImpl* GetCurrentBackendProgram() {
        // P5e (pg), kimi row 107: THE PER-SUB-DRAW ROW. This is reached once per indirect
        // sub-draw for the gl_DrawID / gl_BaseVertex / gl_BaseInstance uniforms, and before P5e
        // every one of those pulled GetProgramForDraw() and, on a stash miss, probed the client
        // allocator. On the handle arm the stash key is the applier's own DrawProgram and the
        // miss path is the by-handle resolver, which is record-first and names no frontend
        // identity - so the whole entry point leaves the frontend.
        const MG_Pipe::MGPipeHandle cso = MG_Pipe::MGPipeApplier().DrawProgram;
        if (MG_Pipe::MGPipeHandleIsNull(cso)) return nullptr;
        const auto* const record = PrgramImpl::FindShaderCsoRecord(cso);
        if (record == nullptr || record->Desc.LinkStatus == 0 || record->Desc.SpirvStatus == 0) {
            return nullptr;
        }
        if (PrgramImpl::g_currentDrawProgramHandle == cso) {
            return PrgramImpl::g_currentDrawBackendProgram;
        }
        return PrgramImpl::ResolveProgramTwin(cso);
    }

    void SetCurrentBaseInstance(Uint32 baseInstance) {
        if (const auto program = GetCurrentBackendProgram()) {
            program->SetBaseInstance(baseInstance);
        }
    }

    void SetCurrentDrawID(Uint32 drawId) {
        if (const auto program = GetCurrentBackendProgram()) {
            program->SetDrawID(drawId);
        }
    }

    void SetCurrentBaseVertex(Int32 baseVertex) {
        if (const auto program = GetCurrentBackendProgram()) {
            program->SetBaseVertex(baseVertex);
        }
    }

    Bool CurrentProgramReadsDrawID() {
        const auto program = GetCurrentBackendProgram();
        return program != nullptr && program->ReadsDrawID();
    }

    Bool CurrentProgramReadsBaseVertex() {
        const auto program = GetCurrentBackendProgram();
        return program != nullptr && program->ReadsBaseVertex();
    }

    // The two questions above, asked from BEFORE PrepareForDraw - where neither can be
    // answered honestly. GetCurrentBackendProgram only sees a twin that a previous draw
    // already synced, and a twin from before a relink still carries the previous link's
    // uniform locations, so "no" there means "not known yet" at least as often as it
    // means no. The multi-draw compute tier has to decide whether to flatten a batch
    // before PrepareForDraw runs (its dispatch cannot come after the draw state), and
    // flattening a batch that turns out to need per-sub-draw values is unrecoverable -
    // so an unanswerable program counts as needing them.
    Bool CurrentProgramMayNeedPerSubDrawBuiltins(Bool batchCarriesBaseVertices) {
        const auto program = GetCurrentBackendProgram();
        if (program == nullptr) return true;
        // P5e (pg), kimi row 108: the same "is this twin current" question, from the record's
        // serial instead of the frontend's link version. The ANSWER TO AN UNANSWERABLE PROGRAM
        // IS STILL `true` - a flattened batch that turns out to need per-sub-draw values is
        // unrecoverable, so "not known yet" has to count as "may need them", and a missing
        // record is the most not-known-yet a program can be.
        const auto* const record = PrgramImpl::FindShaderCsoRecord(MG_Pipe::MGPipeApplier().DrawProgram);
        if (record == nullptr || program->GetSyncedShaderCsoSerial() != record->Serial) {
            return true;
        }
        return program->ReadsDrawID() || (batchCarriesBaseVertices && program->ReadsBaseVertex());
    }
} // namespace MobileGL::MG_Backend::DirectGLES
