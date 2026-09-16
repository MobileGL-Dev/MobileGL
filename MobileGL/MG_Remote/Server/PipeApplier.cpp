// MobileGL - MobileGL/MG_Remote/Server/PipeApplier.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P5 package v1: the applier bridge, and the consumer for contract 7's five class-B verbs.

#include "PipeApplier.h"

#include "../Transport/ReplySlot.h"

#include <Config.h>
#include <MG_Backend/MGPipe/PipeInputs.h>
#include <MG_Remote/Client/ClientSession.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_Util/Converters/GLToMG/TextureEnumConverter.h>
#include <MG_Util/Debug/Log.h>
#include <MG_Util/Metrics/TextureMetrics.h>

#include <cstdlib>
#include <cstring>

namespace MobileGL::MG_Remote::Server {

    ReplyPool::ReplyPool(void* base, Uint64 sizeBytes, Uint32 slotCount, Uint32 slotBytes)
        : m_base(static_cast<Uint8*>(base)), m_size(sizeBytes), m_slots(slotCount), m_slotBytes(slotBytes) {}

    // PACKAGE s1's, not v1's, even though the class is declared in v1's header: the SEG_REPLY
    // slot pool is s1's deliverable (BRIEF 5) and its addressing lives in one place,
    // Transport/ReplySlot.h, which the CLIENT reads the same slots back through. Duplicating
    // `seq % slots` on this side is how the two halves come to disagree about which slot an
    // answer is in - and because seq IS the reply-slot id (R-3), a disagreement reads another
    // call's answer instead of failing.
    //
    // The view is rebuilt per call rather than stored, so that this body does not change
    // ReplyPool's four members and therefore does not touch v1's header at all.
    void ReplyPool::PostReply(Uint64 seq, Int32 status, const void* bytes, Uint64 size) {
        Transport::ReplySlotPool pool(m_base, m_size, m_slots);
        // Fatal inside Post when the answer does not fit a slot: P5 does not chunk replies,
        // and the client knows an answer's size before it emits the record.
        pool.Post(seq, status, bytes, size);
    }

    Uint32 ReplyPool::SlotBytes() const { return m_slotBytes; }

    // -----------------------------------------------------------------------------------
    // ServerVerbSink - the five class-B verbs
    // -----------------------------------------------------------------------------------

    void ServerVerbSink::SetBackend(MG_Backend::BackendObject* backend) { m_backend = backend; }

    const MG_Backend::GlobalBackendFunctionsTable* ServerVerbSink::Table(const char* verb) const {
        if (m_backend == nullptr) {
            // DECLINE BY NAME, DO NOT DEREFERENCE. A verb that arrives before
            // ServerLoop::CreateBackend has run means the hook order changed under us, and the
            // honest answer is "this build did not apply it" - which DecodeAndApply reports as
            // false and the lane sees as a record that did not render, rather than as a crash
            // with no line saying which verb was first.
            MGLOG_E_ONCE("MG_Remote server: %s arrived with no backend object; the verb is "
                         "DECLINED. ServerLoop::CreateBackend runs from MG_Backend::Init()'s "
                         "hook, before ClientSession::Start",
                         verb);
            return nullptr;
        }
        return &m_backend->GetBackendFunctions();
    }

    Bool ServerVerbSink::OnClear(const MG_Pipe::MGPClear& clear) {
        const MG_Backend::GlobalBackendFunctionsTable* table = Table("clear");
        if (table == nullptr) return false;
        const MG_Backend::GLFunctionsTable& gl = table->GL;

        // THE FBO HANDLE IS NOT RESOLVED HERE, AND THAT IS THE RULING RATHER THAN AN OMISSION.
        // MGPClear::Fbo names the framebuffer the clear belongs to, but the BINDING is already
        // server state: set_framebuffer_state (op 33) arrives ahead of the clear and the
        // applier has bound it. Re-resolving the handle to a frontend FramebufferObject here
        // would need the SharedPtr the four ClearNamedFramebuffer* entries take - a frontend
        // heap reference that table 2 lists as one of the six fields with no wire carrier. So
        // P5 clears THE BOUND FRAMEBUFFER, which for the reduced path (default FBO) is exactly
        // right, and the named form is P7's along with the handle it needs.
        switch (clear.Kind) {
        case kMGPClearKindWhole:
            if (gl.Clear == nullptr) return false;
            gl.Clear(static_cast<GLbitfield>(clear.BufferMask));
            break;
        case kMGPClearKindColor:
            switch (clear.ValueClass) {
            case kMGPClearValueClassFloat:
                if (gl.ClearBufferfv == nullptr) return false;
                gl.ClearBufferfv(GL_COLOR, clear.DrawBufferIndex,
                                 reinterpret_cast<const GLfloat*>(clear.ColorValue));
                break;
            case kMGPClearValueClassInt:
                if (gl.ClearBufferiv == nullptr) return false;
                gl.ClearBufferiv(GL_COLOR, clear.DrawBufferIndex,
                                 reinterpret_cast<const GLint*>(clear.ColorValue));
                break;
            case kMGPClearValueClassUint:
                if (gl.ClearBufferuiv == nullptr) return false;
                gl.ClearBufferuiv(GL_COLOR, clear.DrawBufferIndex,
                                  reinterpret_cast<const GLuint*>(clear.ColorValue));
                break;
            default:
                // A value class outside the three is a wire fault, not a fallback: all three
                // representations of a clear colour are numerically populated by the frontend
                // and only this field says which one the backend must use, so guessing renders
                // a plausible wrong colour.
                Wire::WireProtocolFatalAt("MGPClear::ValueClass", clear.ValueClass, 3);
            }
            break;
        case kMGPClearKindDepth:
            if (gl.ClearBufferfv == nullptr) return false;
            gl.ClearBufferfv(GL_DEPTH, 0, &clear.DepthValue);
            break;
        case kMGPClearKindStencil:
            if (gl.ClearBufferiv == nullptr) return false;
            gl.ClearBufferiv(GL_STENCIL, 0, &clear.StencilValue);
            break;
        case kMGPClearKindDepthStencil:
            if (gl.ClearBufferfi == nullptr) return false;
            gl.ClearBufferfi(GL_DEPTH_STENCIL, 0, clear.DepthValue, clear.StencilValue);
            break;
        default:
            Wire::WireProtocolFatalAt("MGPClear::Kind", clear.Kind, kMGPClearKindDepthStencil + 1);
        }
        ++m_clears;
        return true;
    }

    Bool ServerVerbSink::OnBlit(const MG_Pipe::MGPBlit& blit) {
        const MG_Backend::GlobalBackendFunctionsTable* table = Table("blit");
        if (table == nullptr) return false;
        if (table->GL.BlitFramebuffer == nullptr) return false;
        // Same ruling as OnClear's: the read and draw framebuffers are already bound by the
        // set_framebuffer_state records that preceded this one, so the unnamed entry point is
        // the one that matches what the server's state actually is. BlitNamedFramebuffer needs
        // two frontend SharedPtrs, which table 2 lists as uncarried.
        table->GL.BlitFramebuffer(blit.SrcX0, blit.SrcY0, blit.SrcX1, blit.SrcY1, blit.DstX0,
                                  blit.DstY0, blit.DstX1, blit.DstY1,
                                  static_cast<GLbitfield>(blit.Mask),
                                  static_cast<GLenum>(blit.Filter));
        ++m_blits;
        return true;
    }

    Bool ServerVerbSink::OnPresent(const MG_Pipe::MGPPresent& present) {
        const MG_Backend::GlobalBackendFunctionsTable* table = Table("present");
        if (table == nullptr) return false;
        if (table->Present == nullptr) return false;
        // Present is the ONLY frame-boundary drain the backend has (DirectGLES.cpp:12424-12470:
        // the fence poll, the four ring OnPresent hooks, TrimBufferPool, PipeStats::OnPresent),
        // which is why ARCHITECTURE.md:531 wants present <-> eglSwapBuffers to stay 1:1.
        //
        // m-1: THAT 1:1 IS A CONVENTION c1 UPHOLDS, NOT A STRUCTURAL GUARANTEE, and the earlier
        // claim that it was structural is wrong. This Present() is reached ONLY from a present
        // RECORD (ServerVerbSink::OnPresent). ServerSwapEGLBuffers does NOT reach it - it calls
        // backend->SwapEGLBuffers -> BackendObject::SwapEGLBuffers -> eglSwapBuffers, and never
        // Present() - so the two paths do NOT both end here. The frame count staying in step with
        // the swap count rests entirely on c1 emitting exactly one present record per swap;
        // nothing here compares Presents() to a swap count. If that drifts, the frame fence and
        // TrimBufferPool's recycle watermark stop tracking frames - which is the reason the 1:1
        // was wanted, recorded here so a future swap-without-present is looked for rather than
        // assumed impossible. Presents() is exposed for a lane that wants to make the comparison.
        table->Present();
        ++m_presents;
        // FrameSerial 0 means "the server stamps its own" (c1-v1 8.3): P5 has no client-side
        // present credit, so the client sends 0 and the frame count on this side IS the serial.
        m_lastPresentSerial = present.FrameSerial != 0 ? present.FrameSerial : m_presents;
        return true;
    }

    Bool ServerVerbSink::OnReadPixels(const MG_Pipe::MGPReadbackInfo& info, Uint64 seq,
                                      Wire::ReplySink* replies) {
        if (replies == nullptr) {
            // The decoder always passes its ReplySink; a null one means the applier was built
            // without a reply pool, and answering nothing would leave the client's barrier
            // waiting for a slot that never gets stamped - a hang, not a wrong picture.
            MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"read_pixels without a reply sink\"} - "
                    "the pixels' only destination in P5 is SEG_REPLY (contract table 1 row 23) "
                    "and a client blocked on seq %llu would never be answered",
                    static_cast<unsigned long long>(seq));
            std::abort();
        }
        const MG_Backend::GlobalBackendFunctionsTable* table = Table("read_pixels");
        if (table == nullptr || table->GL.ReadPixels == nullptr) {
            // DECLINED IS A REAL ANSWER (table 0's slot-header row) and it is the RIGHT one
            // here: the client is parked on this seq inside the verb barrier, so returning
            // false without posting would convert "not implemented" into "never returns".
            replies->PostReply(seq, Wire::ReplySink::kStatusDeclined, nullptr, 0);
            return false;
        }
        if (info.DstSize == 0) {
            replies->PostReply(seq, Wire::ReplySink::kStatusError, nullptr, 0);
            return false;
        }
        // ID-49: THE REPLY CROSSES TIGHT AND PACK STATE NEVER CROSSES FOR A READ. The server reads
        // with NEUTRAL pack state - ROW_LENGTH 0, SKIP_ROWS/PIXELS/IMAGES 0, ALIGNMENT 1 - into a
        // w*h*bytesPerPixel extent that IS the reply payload, and restores the pack state
        // afterwards; the CLIENT scatters those tight rows into the application's pointer per its
        // own GL_PACK_* state (c1's half). Reading with the client's pack state HERE was the
        // codex-1 blocker: the backend's ReadPixels honours ROW_LENGTH/SKIP_* and writes PAST a
        // DstSize the client sized without the initial skip (a 4x3 RGBA8 read with ROW_LENGTH=8,
        // SKIP_ROWS=1, SKIP_PIXELS=2 allocates 80 and lands its last write at 120), which is the
        // two DepthReadbackHonoursThePackPixelStoreParameters SEGFAULTs the census omitted. THE
        // DstSize FORMULA BOTH SIDES AGREE ON: w * h * bytesPerPixel. A non-default server-visible
        // pack state can no longer change either the reply's size or its bytes.
        const SizeT bytesPerPixel = MG_Util::GetInputBytesPerPixel(
            MG_Util::ConvertGLEnumToTextureInputFormat(static_cast<GLenum>(info.Format)),
            MG_Util::ConvertGLEnumToTexturePixelDataType(static_cast<GLenum>(info.Type)));
        // Tight size = w*h*bpp, the whole of ID-49's formula. If this build cannot size the
        // (format, type) pair (bpp == 0) it trusts the client's DstSize - a neutral read still
        // cannot overflow it via row length or skips, and an unsizeable pair is c1's
        // Fatal{UnsizedReadback} at emission, not this side's.
        const Uint64 tight = bytesPerPixel != 0
                                 ? static_cast<Uint64>(info.Box.W) * static_cast<Uint64>(info.Box.H) *
                                       static_cast<Uint64>(bytesPerPixel)
                                 : info.DstSize;
        if (bytesPerPixel != 0 && tight != info.DstSize) {
            // Both halves compute w*h*bpp under ID-49, so a disagreement is the two sides
            // disagreeing about the frame. Read (and post) the tight extent this side owns rather
            // than the client's number, so a wrong DstSize can never make this a short read into
            // uninitialised scratch.
            MGLOG_E_ONCE("MG_Remote server: read_pixels DstSize %llu != tight w*h*bpp %llu "
                         "(%ux%u, bpp %zu); reading the tight extent (ID-49)",
                         static_cast<unsigned long long>(info.DstSize),
                         static_cast<unsigned long long>(tight), info.Box.W, info.Box.H,
                         bytesPerPixel);
        }
        if (tight > m_readbackScratch.size()) {
            m_readbackScratch.resize(static_cast<SizeT>(tight));
        }

        // Save the server-visible pack state, force neutral for the read, restore. Both go through
        // the applier's own set_pixel_pack_state entry point (MGPipeApplySetPixelPackState writes
        // gPipeInputs.m_pixelStore[0], which the backend's ReadPixels reads via
        // MGB_CTX->GetPixelStoreParameters); the read is synchronous on this thread, so the window
        // in which the pack state is neutral does not outlive the call.
        const MG_Pipe::PixelStoreParameters savedPack =
            MG_Pipe::gPipeInputs.GetPixelStoreParameters(/*isUnpack=*/false);
        MG_Pipe::MGPPixelPackState neutralPack{};
        neutralPack.Pack.RowLength = 0;
        neutralPack.Pack.SkipRows = 0;
        neutralPack.Pack.SkipPixels = 0;
        neutralPack.Pack.SkipImages = 0;
        neutralPack.Pack.Alignment = 1;
        MG_Pipe::MGPipeApplySetPixelPackState(neutralPack);

        table->GL.ReadPixels(info.Box.X, info.Box.Y, static_cast<GLsizei>(info.Box.W),
                             static_cast<GLsizei>(info.Box.H), static_cast<GLenum>(info.Format),
                             static_cast<GLenum>(info.Type), m_readbackScratch.data());

        MG_Pipe::MGPPixelPackState restorePack{};
        restorePack.Pack = savedPack;
        MG_Pipe::MGPipeApplySetPixelPackState(restorePack);

        // m-7: the answer is written into the slot HERE, mid-apply, while the verb stamp is still
        // up - and that is safe for exactly one reason, which is the contract's and is stated so it
        // is not mistaken for luck: the client reaches a reply slot ONLY through appliedSeq
        // (ReplySlot.h's ORDERING clause), never by polling the slot's own stamp, and s1's
        // SessionConsumer::ApplyOne publishes appliedSeq only AFTER PipeApplier::ApplyOne has run
        // LeaveApplier() and (on the joint tree) dropped the ScopedApplierEntry. So by the time the
        // client is allowed to look at this slot, the apply-side gPipeInputs flag is already down.
        replies->PostReply(seq, Wire::ReplySink::kStatusOk, m_readbackScratch.data(), tight);
        m_readbackBytes += tight;
        ++m_readbacks;
        return true;
    }

    Bool ServerVerbSink::OnDrawVbo(const MG_Pipe::MGPDrawInfo& info,
                                   const MG_Pipe::MGPDrawRange* ranges,
                                   const MG_Pipe::MGHostSpan* userIndices) {
        const MG_Backend::GlobalBackendFunctionsTable* table = Table("draw_vbo");
        if (table == nullptr) return false;
        if (userIndices != nullptr) {
            // kCapNeedsHostIndexBytes is 0 for the whole of P5 by ruling (table 0's cap-bit
            // row) precisely so this tail never appears; a span that arrived anyway means the
            // client's cap gate did not hold, and filling one is P8's.
            MGLOG_E_ONCE("MG_Remote server: draw_vbo carries an MGHostSpan of user indices. P5 "
                         "rules kCapNeedsHostIndexBytes and kCapNeedsHostUboBytes to 0 so that "
                         "no host span reaches the first IPC frame (contract table 0); filling "
                         "one under split is P8's. The draw is DECLINED rather than drawn from "
                         "a pointer that does not belong to this process");
            return false;
        }
        if (ranges == nullptr || info.NumDraws == 0) return false;

        // P5 IMPLEMENTS THE TWO SHAPES ITS REDUCED PATH USES AND DECLINES THE REST BY NAME.
        // draw_vbo collapses all twenty draw entry points, and picking the right one needs the
        // instancing / base-vertex / base-instance / multi-draw cross product. TriangleScenario
        // is a single non-instanced array draw and OpenRA's are single indexed draws from a
        // bound element buffer; the rest are P8's, together with the MGPDrawIndirect record
        // that has no producer yet.
        const MG_Backend::GLFunctionsTable& gl = table->GL;
        const Bool instanced = info.InstanceCount > 1 || info.StartInstance != 0;
        if (info.NumDraws != 1 || instanced) {
            MGLOG_E_ONCE("MG_Remote server: draw_vbo with NumDraws=%u InstanceCount=%u "
                         "StartInstance=%u is DECLINED - P5's reduced path is the single "
                         "non-instanced draw (BRIEF 4); the multi-draw and instanced arms are "
                         "P8's",
                         info.NumDraws, info.InstanceCount, info.StartInstance);
            return false;
        }
        const MG_Pipe::MGPDrawRange& range = ranges[0];
        if (info.IndexSize == 0) {
            if (gl.DrawArrays == nullptr) return false;
            gl.DrawArrays(static_cast<GLenum>(info.Mode), static_cast<GLint>(range.Start),
                          static_cast<GLsizei>(range.Count));
        } else {
            if (gl.DrawElementsBaseVertex == nullptr) return false;
            GLenum indexType = GL_UNSIGNED_INT;
            switch (info.IndexSize) {
            case 1: indexType = GL_UNSIGNED_BYTE; break;
            case 2: indexType = GL_UNSIGNED_SHORT; break;
            case 4: indexType = GL_UNSIGNED_INT; break;
            default:
                // IndexSize is "0 = arrays, else 1 / 2 / 4" (MGPipeTypes.h:1323) and nothing
                // else is a legal width; defaulting to 4 would read past the element buffer.
                Wire::WireProtocolFatalAt("MGPDrawInfo::IndexSize", info.IndexSize, 4);
            }
            // Start is the FIRST INDEX, so the byte offset into the bound element buffer is
            // Start * IndexSize - the same arithmetic PipeFill's emitter inverted.
            const auto offset = static_cast<std::uintptr_t>(range.Start) * info.IndexSize;
            gl.DrawElementsBaseVertex(static_cast<GLenum>(info.Mode),
                                      static_cast<GLsizei>(range.Count), indexType,
                                      reinterpret_cast<const void*>(offset), range.IndexBias);
        }
        ++m_draws;
        return true;
    }

    // -----------------------------------------------------------------------------------
    // PipeApplier
    // -----------------------------------------------------------------------------------

    PipeApplier::PipeApplier(Wire::SegmentTable* segments, ReplyPool* replies)
        : m_segments(segments), m_replies(replies) {}

    void PipeApplier::Attach(Transport::RingControl* control, MG_Backend::BackendObject* backend) {
        if (control == nullptr || m_segments == nullptr) {
            MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"PipeApplier::Attach\"} - no control "
                    "page or no segment table; ServerSession::Accept builds both before the "
                    "apply thread starts");
            std::abort();
        }
        m_verbs.SetBackend(backend);
        m_decoder = Wire::PipeWireDecoder(control, m_segments, m_replies);
        m_decoder.SetVerbSink(&m_verbs);
        m_attached = true;
    }

    void PipeApplier::Detach() {
        m_decoder = Wire::PipeWireDecoder();
        m_verbs.SetBackend(nullptr);
        m_attached = false;
    }

    Bool PipeApplier::Attached() const { return m_attached; }

    Bool PipeApplier::ApplyOne(const Transport::RingRecordView& record) {
        if (!m_attached) {
            MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"PipeApplier::ApplyOne before Attach\"} "
                    "- a record reached the applier with no decoder; the apply thread calls "
                    "Attach once before its first pop");
            std::abort();
        }
        // R-1's INVARIANT, THE SERVER'S HALF (table 3's gPipeInputs row, c1-v1 8.1). The flag
        // is raised for the WHOLE of this function and not only around DecodeAndApply: the
        // stamp below and LeaveApplier at the end are both writes to gPipeInputs, and the client
        // asserts the flag is down before it publishes (ClientSession::EmitAndWait), so a
        // bracket that excluded either would leave a real write outside the check. It is
        // dropped before this function returns, and s1's SessionConsumer::ApplyOne publishes
        // appliedSeq only after that - so by the time the client is runnable the flag is down.
        const Client::ClientSession::ScopedApplierEntry insideApplier;
        // ORDER IS THE CONTRACT'S: stamp, then apply. The stamp is what makes any server-side
        // read of gPipeInputs legal at all (PipeApplier.h's block 1), so a record applied
        // before it aborts on the FIRST field inside SyncRenderState.
        StampVerbBoundary(static_cast<MG_Pipe::MGPWireOp>(record.kind));
        const Bool applied = m_decoder.DecodeAndApply(record);
        // AND THE CLEAR IS INSIDE ApplyOne, NOT AFTER THE DRAIN BATCH. That is not tidiness,
        // it is the barrier invariant. s1's SessionConsumer::ApplyOne publishes appliedSeq the
        // instant this returns, and publishing appliedSeq is what makes the CLIENT runnable
        // again (R-1: the barrier waits on exactly that watermark). A clear that ran after the
        // batch would therefore be a second writer of gPipeInputs while the client is already
        // touching it - the one thing table 3 says may not be introduced before the barrier
        // retires - and the first version of this file had it there. It was caught by
        // AClearRecordCrossesAndIsStampedAsAVerbBoundary failing INTERMITTENTLY, which is what
        // a race looks like from the outside.
        //
        // THE COST, STATED: a record that is NOT a verb boundary now applies with the flag
        // disarmed, so a sticky forward pulled from inside such a record's applier is not
        // counted in `rsp`. Closing that needs an "enter the applier" entry point beside
        // MGPipeServerStampVerbBoundary that arms the flag WITHOUT re-stamping - re-stamping on
        // a non-verb op is what p1 forbids outright - and PipeInputs.cpp is p1's file. Left for
        // the integrator to sequence; it makes `rsp` larger, never smaller, so the number this
        // phase reports is a floor.
        LeaveApplier();
        return applied;
    }

    // p1's rule verbatim (p1-v1 2). MGPipeVerbForWireOp is generated from MGP_VERB_OP_LIST in
    // FieldOwnership.def and answers kVerbCount for every op that is NOT a verb boundary, so
    // calling it unconditionally on every record is both correct and cheap. Four ops stamp:
    // Clear -> Clear, DrawVbo -> DrawArrays, ReadPixels -> ReadPixels, Blit -> BlitFramebuffer.
    //
    // PRESENT IS DELIBERATELY NOT ONE, although contract 7 puts it in class B: FillPoints.def:21
    // says Present and SetSwapInterval "go through BackendObject virtuals and read no frontend
    // state, so they are not verbs here". There is no MGPipeVerb::Present, and stamping there
    // would retire the previous verb's answers with nothing to put in their place.
    void PipeApplier::StampVerbBoundary(MG_Pipe::MGPWireOp op) {
        const MG_Pipe::MGPipeVerb verb = MG_Pipe::MGPipeVerbForWireOp(op);
        if (verb == MG_Pipe::MGPipeVerb::kVerbCount) return; // not a verb boundary: stamp nothing
        MG_Pipe::MGPipeServerStampVerbBoundary(verb);
    }

    void PipeApplier::LeaveApplier() { MG_Pipe::MGPipeServerClearVerbBoundary(); }

    Uint64 PipeApplier::ResidualPullCount() const { return MG_Pipe::MGPipeResidualPullCount(); }

    // The decoder poisons EXACTLY the runs it resolved, from inside DecodeAndApply, once the
    // applier has returned - so this entry point is the manual one, for a caller that knows a
    // range is dead and is not the decoder. It is kept because c0's signature block declares
    // it and because the R-11 copy in Managers.cpp is verified by poisoning a range by hand in
    // a unit case; nothing on the live path calls it.
    void PipeApplier::PoisonRetiredStageBytes(Uint64 offset, Uint64 size) {
        if (size == 0 || m_segments == nullptr) return;
#if MOBILEGL_BUILD_DISAGGREGATED
        if (!MG_Config::Ipc.Audit) return;
#endif
        const void* run = m_segments->Resolve(Wire::kSegStage, offset, size);
        if (run == nullptr) return;
        std::memset(const_cast<void*>(run), 0xDD, static_cast<SizeT>(size));
    }

    Uint64 PipeApplier::PoisonedStageBytes() const { return m_decoder.PoisonedStageBytes(); }

    Uint64 PipeApplier::DecoderAppliedSeq() const { return m_decoder.AppliedSeq(); }

} // namespace MobileGL::MG_Remote::Server
