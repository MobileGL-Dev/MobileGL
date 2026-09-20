// MobileGL - MobileGL/MG_Remote/Server/PipeApplier.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P5 package v1: the applier bridge, and the consumer for contract 7's five class-B verbs.

#include "PipeApplier.h"

#include "ServerSession.h"
#include "../Transport/ReplySlot.h"

#include <Config.h>
#include <MG_Backend/MGPipe/PipeInputs.h>
// P5c ct: object_death's per-kind release names the Espryt twin tables (CONTRACT-P5C.md
// §5.2). The same dependency ServerLoop.cpp already takes for CreateBackend; a server built
// on Magma simply holds no twins in these tables and every release resolves to nothing.
#include <MG_Backend/DirectGLES/Managers.h>
#include <MG_Backend/DirectGLES/DirectGLES.h>
#include <MG_Backend/DirectVulkan/DirectVulkan.h>
#include <MG_Remote/Client/ClientSession.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_Util/Converters/GLToMG/TextureEnumConverter.h>
#include <MG_Util/Debug/Log.h>
#include <MG_Util/Metrics/TextureMetrics.h>

#include <cstdlib>
#include <cstring>

namespace MobileGL::MG_Remote::Server {

    // P5e (ra, CONTRACT-P5E §1 / §6): DOES THIS SERVER PUBLISH kCapRunAheadApply? The
    // question is asked of the server's OWN CallMask and not of a build constant, because
    // "the client may run ahead" is exactly what that bit says and Magma never sets it.
    // A session with no CallMask yet (the bring-up window, a fixture that never called
    // SetCapabilityBits) answers false: no client can have latched run-ahead against a
    // snapshot that was never published.
    //
    // P5e (gl, ID-111): PROMOTED OUT OF THE ANONYMOUS NAMESPACE, unchanged in body. It is now
    // the second conjunct of the barriered stamp as well as Present's frame-serial gate, and
    // the red-once has to be able to assert what it answers for the session it built.
    Bool MGPipeServerPublishesRunAhead() {
        const ServerSession* session = ServerSession::Active();
        if (session == nullptr || !session->CallMaskIsSet()) return false;
        return (session->CallMask() & static_cast<Uint64>(MG_Pipe::kCapRunAheadApply)) != 0;
    }

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

    void ServerVerbSink::SetBackend(MG_Backend::BackendObject* backend) {
        if (m_backend != backend) {
            ReleaseQueries();
            ReleaseFences();
        }
        m_backend = backend;
    }

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

    ServerVerbSink::FenceEntry& ServerVerbSink::FindFence(MG_Pipe::MGPipeHandle handle) {
        const auto it = m_fences.find(handle.Slot);
        if (handle.Slot == 0 || it == m_fences.end() ||
            !it->second.Live || it->second.Gen != handle.Gen) {
            Wire::WireProtocolFatal("Fence.handle", "missing, destroyed or stale fence handle");
        }
        return it->second;
    }

    Bool ServerVerbSink::OnFenceCreate(const MG_Pipe::MGPHandleOnly& desc) {
        if (desc.Kind != static_cast<Uint32>(MG_Pipe::MGPipeKind::Fence))
            Wire::WireProtocolFatal("Fence.Kind", "expected Fence namespace");
        const auto* table = Table("FenceCreate");
        if (table == nullptr) return false;
        const auto handle = desc.Handle;
        if (handle.Slot == 0) {
            Wire::WireProtocolFatal("FenceCreate.handle", "reserved fence handle");
        }
        const Bool seen = m_fences.find(handle.Slot) != m_fences.end();
        auto& entry = m_fences[handle.Slot];
        if (entry.Live || (seen && handle.Gen <= entry.Gen)) {
            Wire::WireProtocolFatal("FenceCreate.handle", "duplicate or stale fence generation");
        }
        entry.Gen = handle.Gen;
        entry.Live = true;
        // GL_Sync.cpp treats an absent slot or a null creation result as always signaled.
        entry.Native = table->GL.FenceSync == nullptr ? nullptr : table->GL.FenceSync();
        return true;
    }

    Bool ServerVerbSink::OnFenceDestroy(const MG_Pipe::MGPHandleOnly& desc) {
        if (desc.Kind != static_cast<Uint32>(MG_Pipe::MGPipeKind::Fence))
            Wire::WireProtocolFatal("Fence.Kind", "expected Fence namespace");
        auto& entry = FindFence(desc.Handle);
        const auto* table = Table("FenceDestroy");
        if (table == nullptr) return false;
        if (entry.Native != nullptr && table->GL.DeleteSync != nullptr) table->GL.DeleteSync(entry.Native);
        entry.Native = nullptr;
        entry.Live = false;
        return true;
    }

    Bool ServerVerbSink::OnFenceStatus(const MG_Pipe::MGPHandleOnly& desc, Uint32& result) {
        if (desc.Kind != static_cast<Uint32>(MG_Pipe::MGPipeKind::Fence))
            Wire::WireProtocolFatal("Fence.Kind", "expected Fence namespace");
        auto& entry = FindFence(desc.Handle);
        const auto* table = Table("FenceStatus");
        if (table == nullptr) return false;
        result = entry.Native == nullptr || table->GL.GetSyncStatus == nullptr ||
                 table->GL.GetSyncStatus(entry.Native);
        return true;
    }

    Bool ServerVerbSink::OnFenceWait(const MG_Pipe::MGPFenceWait& request, Uint32& result) {
        if ((request.Flags & ~static_cast<Uint32>(GL_SYNC_FLUSH_COMMANDS_BIT)) != 0) {
            Wire::WireProtocolFatal("FenceWait.Flags", "unknown client-wait flag");
        }
        auto& entry = FindFence(request.Fence);
        const auto* table = Table("FenceWait");
        if (table == nullptr) return false;
        result = entry.Native == nullptr || table->GL.ClientWaitSync == nullptr
                     ? GL_ALREADY_SIGNALED
                     : table->GL.ClientWaitSync(entry.Native, request.Flags, request.TimeoutNs);
        return true;
    }

    Bool ServerVerbSink::OnFenceWaitServer(const MG_Pipe::MGPFenceWait& request) {
        if (request.Flags != 0 || request.TimeoutNs != GL_TIMEOUT_IGNORED) {
            Wire::WireProtocolFatal("FenceWaitServer.arguments", "invalid server wait arguments");
        }
        auto& entry = FindFence(request.Fence);
        const auto* table = Table("FenceWaitServer");
        if (table == nullptr) return false;
        if (entry.Native != nullptr && table->GL.WaitSync != nullptr)
            table->GL.WaitSync(entry.Native, request.Flags, request.TimeoutNs);
        return true;
    }

    void ServerVerbSink::ReleaseFences() {
        // Detach runs on the apply thread before its private backend/context is destroyed.
        if (m_backend != nullptr) {
            const auto destroy = m_backend->GetBackendFunctions().GL.DeleteSync;
            if (destroy != nullptr) {
                for (auto& [slot, entry] : m_fences)
                    if (entry.Live && entry.Native != nullptr) destroy(entry.Native);
            }
        }
        m_fences.clear();
    }

#include "QueryServer.inc"

    Bool ServerVerbSink::OnClear(const MG_Pipe::MGPClear& clear) {
        const MG_Backend::GlobalBackendFunctionsTable* table = Table("clear");
        if (table == nullptr) return false;
        const MG_Backend::GLFunctionsTable& gl = table->GL;

        // The named framebuffer record already crossed before this verb. Scope
        // only the server's resolved draw target; the next verb still observes
        // the application's unchanged binding. Both backends sync from this
        // handle and never need the frontend FramebufferObject.
        auto& applier = MG_Pipe::MGPipeApplier();
        struct ScopedClearTarget {
            MG_Pipe::MGPipeApplierState& State;
            MG_Pipe::MGPipeHandle Saved;
            ~ScopedClearTarget() { State.BoundFramebuffer[0] = Saved; }
        } clearTarget{applier, applier.BoundFramebuffer[0]};
        if (!MG_Pipe::MGPipeHandleIsNull(clear.Fbo)) {
            if (!applier.FramebufferRecordFor(clear.Fbo))
                Wire::WireProtocolFatal("Clear.Fbo", "missing named framebuffer record");
            applier.BoundFramebuffer[0] = clear.Fbo;
        }
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
#if MOBILEGL_BUILD_DISAGGREGATED
        // P5c (hd, CONTRACT-P5C §3.3): the record's handles cross to the backend as the verb's
        // own state. The bound form carries two nulls and nothing changes; the named form's
        // pair is what the backend's named-blit arm resolves - the sink no longer relies on
        // "the read and draw framebuffers are already bound" (the client's ScopedBlitBindings
        // staging is deleted with this), and a backend that does not consume the pair has no
        // named arm, which is a loud decline rather than a blit of whatever is bound.
        auto& applierState = MG_Pipe::MGPipeApplier();
        applierState.ClearVerbHandles();
        applierState.VerbBlitReadFbo = blit.ReadFbo;
        applierState.VerbBlitDrawFbo = blit.DrawFbo;
        const Bool named = !MG_Pipe::MGPipeHandleIsNull(blit.ReadFbo) ||
                           !MG_Pipe::MGPipeHandleIsNull(blit.DrawFbo);
#endif
        table->GL.BlitFramebuffer(blit.SrcX0, blit.SrcY0, blit.SrcX1, blit.SrcY1, blit.DstX0,
                                  blit.DstY0, blit.DstX1, blit.DstY1,
                                  static_cast<GLbitfield>(blit.Mask),
                                  static_cast<GLenum>(blit.Filter));
#if MOBILEGL_BUILD_DISAGGREGATED
        if (named && !applierState.VerbBlitNamedConsumed) {
            MGLOG_E_ONCE("MGPipe: a named blit (read {%u, %u}, draw {%u, %u}) reached a backend "
                         "with no named-blit arm; the verb is DECLINED rather than applied to "
                         "the bound framebuffers",
                         blit.ReadFbo.Slot, blit.ReadFbo.Gen, blit.DrawFbo.Slot, blit.DrawFbo.Gen);
            applierState.VerbBlitReadFbo = MG_Pipe::kMGPipeNullHandle;
            applierState.VerbBlitDrawFbo = MG_Pipe::kMGPipeNullHandle;
            return false;
        }
#endif
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
        //
        // P5e (ra, CONTRACT-P5E §1, §2.4): IT IS THE CLIENT'S NOW, 1-BASED AND MINTED BY THE
        // PAYER. A credit can only be paced in an id space the waiter advances, and the waiter
        // is the client (`WaitForPresentAck(m_presentsSent + 1 - credit)`), so a server-stamped
        // serial would be the server acknowledging its own count. The 0 arm survives for a
        // peer that has not been re-built - and on a server that PUBLISHES the run-ahead cap it
        // is a protocol fault, because such a client is pacing on this answer and a 0 would
        // acknowledge a frame nobody asked about.
        if (present.FrameSerial == 0 && MGPipeServerPublishesRunAhead()) {
            MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"Present.FrameSerial\"} - a run-ahead "
                    "server was handed present serial 0. The client mints this 1-based and "
                    "waits on it for its credit (CONTRACT-P5E §2.4); returning a credit for "
                    "serial 0 would release a wait that is asking about frame N");
            std::abort();
        }
        m_lastPresentSerial = present.FrameSerial != 0 ? present.FrameSerial : m_presents;
        // §2.4's other half, and the reason ServerSession::ReturnPresentCredit has had no
        // production caller since v1 wrote it: ONE CREDIT PER SWAP, returned after Present()
        // has returned rather than before it, because what the client is waiting for is the
        // swap and not the record's apply. It advances presentAckSerial AND rings the client's
        // bell - a client parked in WaitForPresentAck(kWaitForever) needs the pair.
        if (ServerSession* session = ServerSession::Active()) {
            session->ReturnPresentCredit(m_lastPresentSerial);
        }
        return true;
    }

    Bool ServerVerbSink::OnGetTextureImage(const MG_Pipe::MGPReadbackInfo& info, Uint64 seq,
                                           Wire::ReplySink* replies) {
        if (!replies) Wire::WireProtocolFatal("GetTextureImage.reply", "missing reply sink");
        const SizeT bpp = MG_Util::GetInputBytesPerPixel(
            MG_Util::ConvertGLEnumToTextureInputFormat(info.Format),
            MG_Util::ConvertGLEnumToTexturePixelDataType(info.Type));
        if (!bpp || !info.Box.W || !info.Box.H || !info.Box.D || info.Box.X || info.Box.Y || info.Box.Z) {
            Wire::WireProtocolFatal("GetTextureImage.extent", "invalid tight image extent");
        }
        const Uint64 tight = static_cast<Uint64>(info.Box.W) * info.Box.H * info.Box.D * bpp;
        if (tight / bpp / info.Box.W / info.Box.H != info.Box.D ||
            info.DstOffset > tight || !info.DstSize || info.DstSize > tight - info.DstOffset)
            Wire::WireProtocolFatal("GetTextureImage.range", "invalid reply window");
        auto image = info;
        image.DstOffset = 0;
        image.DstSize = tight;
        Vector<Uint8> bytes;
        Bool ok = false;
#if MOBILEGL_BUILD_DISAGGREGATED
        if (m_backend && m_backend->GetBackendType() == BackendType::DirectGLES)
            ok = MG_Backend::DirectGLES::ReadTextureImageWire(image, bytes);
        else if (m_backend && m_backend->GetBackendType() == BackendType::DirectVulkan &&
                 MG_Backend::DirectVulkan::pVulkanRenderer)
            ok = MG_Backend::DirectVulkan::pVulkanRenderer->ReadTextureImageWire(image, bytes);
#endif
        if (!ok || bytes.size() != tight) {
            replies->PostReply(seq, Wire::ReplySink::kStatusError, nullptr, 0);
            return false;
        }
        replies->PostReply(seq, Wire::ReplySink::kStatusOk, bytes.data() + info.DstOffset, info.DstSize);
        m_readbackBytes += info.DstSize;
        ++m_readbacks;
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

    // P5b's server-side stub shape (CONTRACT-P5B.md): the same line the client's class-C table
    // raises (EmitTables.cpp UnmigratedVerbFatal) and the same family the census greps, so a
    // slot flipped on the client ahead of its server half aborts BY NAME on the apply thread
    // rather than rendering nothing. Named "(server sink)" in the message so a log reader can
    // tell which half is missing.
    [[noreturn]] static void ServerUnmigratedVerbFatal(const char* slot) {
        MGLOG_F("MGPipe: Fatal{UnmigratedVerb, \"%s\"} (server sink: the record crossed and "
                "ServerVerbSink has no body for it yet - CONTRACT-P5B.md names the package)",
                slot);
        std::abort();
    }

    // P5b d1 (MG_Remote/CONTRACT-P5B.md §2 d1): draw_vbo's whole cross product. The record
    // carries the GL call verbatim (rule D) and this body reproduces the backend call the
    // monolith makes for that shape, reading only the record and the backend's own
    // barrier-pulled state; the twenty GL entry points collapse onto the arms below:
    //
    //   kDrawIsIndirect      arrays: DrawArraysIndirect (DrawCount 1, Stride 0) /
    //                        MultiDrawArraysIndirect / MultiDrawArraysIndirectCount (a parameter
    //                        buffer named); indexed: the three Elements twins
    //   NumDraws != 1        MultiDrawArrays / MultiDrawElementsBaseVertex, the two arrays
    //                        rebuilt from the ranges into bounded locals (rule C)
    //   arrays, one range    DrawArrays / DrawArraysInstanced / DrawArraysInstancedBaseInstance
    //   indexed, one range   DrawElementsBaseVertex (the P5 arm, unchanged, bias 0 for a plain
    //                        DrawElements) / DrawRangeElements[BaseVertex] under
    //                        kDrawHasIndexRange / the four DrawElementsInstanced* by whether a
    //                        base vertex and a base instance are non-zero
    //   kDrawHasUserIndices  the `indices` argument is the resolved SEG_STAGE run instead of an
    //                        element-buffer offset (the client staged a client index array)
    //
    // "Instanced" is InstanceCount != 1 || StartInstance != 0: an instanced call with a count
    // of 1 and no base instance is the plain draw it is equivalent to, and a count of 0 must
    // NOT collapse onto the plain draw (it draws nothing, the plain draw would draw once).
    //
    // What is still refused by name (the census's own grep family): a multi-draw that arrived
    // with a span (the client refuses "MultiDrawElements+CLIENT_INDICES" first; P8's
    // HostResolve.cpp flattens it) and a multi-draw that claims instancing (no GL entry point
    // produces one; the client never sends it).
#if MOBILEGL_BUILD_DISAGGREGATED
    namespace {
        // P5e (tx2), CONTRACT-P5E §5.3 / ruling 19 (ID-95, A8 closed). THE TWO UNIT WINDOWS MUST
        // COVER [0, MaxTouchedTextureUnit], AND THIS IS WHERE THAT PROMISE IS CHECKED.
        //
        // Everything the texture and sampler families do per draw now reads
        // [SamplerViewStart, +SamplerViewCount) and [SamplerStateStart, +SamplerStateCount).
        // Under run-ahead a window NARROWER than the frontend's high-water mark silently drops a
        // sync of a texture the draw is about to sample, or leaves an earlier draw's sampler
        // object on a unit - and a decline is exactly what run-ahead cannot take, because the
        // client has already moved on and there is no wait in which to notice.
        //
        // A SERVER-SIDE RE-DERIVATION IS REFUSED, and that is the ruling's point: the client owns
        // the high-water mark (it is the `count` argument SamplerEmit.h passes, Start=0 /
        // Count=maxTouched+1) and the server's job is to check the promise, not to invent a
        // second authority for it. So this is a CHECK and its failure is corruption.
        //
        // WHERE THE MARK COMES FROM: MGPContextValues::MaxTouchedTextureUnit, applied by
        // set_context_values, which precedes the verb on the ring - the same ordering §2.1's
        // XFB clause leans on. It is RECORD_SUPPLIED, so reading it is reading what a record
        // put there and not client memory (rule F).
        //
        // WIDER IS FINE. A window larger than the mark costs a walk over provably-empty units;
        // only SMALLER is unrepresentable. An empty window on a draw that touches no unit at all
        // (mark 0 with nothing ever touched) is the correct and only possible emission, so a
        // count of 0 is admitted exactly while the applied counters say no unit was touched.
        //
        // THE ONE ADMITTED SILENCE, and it is the A/B rather than a hole: a client whose sampler
        // subsystem bit is CLEAR emits neither record, the backend then runs its pre-handle
        // frontend walk, and there is no window to check because there is no window. That is
        // "both counts are still 0", and it is distinguishable from the narrowing this refuses
        // (a narrowed window carries a non-zero count that is merely too small). The moment
        // either set has been received, the rule binds.
        void CheckUnitWindows(const MG_Pipe::MGPipeApplierState& st, const char* verb) {
            if (st.SamplerViewCount == 0 && st.SamplerStateCount == 0) return;
            const Int maxTouched = MG_Pipe::gPipeInputs.GetMaxTouchedTextureUnit();
            if (maxTouched < 0) return;
            const Uint32 required = static_cast<Uint32>(maxTouched) + 1u;
            if (st.SamplerViewStart != 0 || st.SamplerViewCount < required) {
                MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"SetSamplerViews.Count\"} - %s applies with "
                        "units 0..%d touched, so set_sampler_views must carry Start=0 and Count >= %u "
                        "(CONTRACT-P5E.md §5.3); the applied window is Start=%u Count=%u, which drops "
                        "the sync of at least one texture this draw samples",
                        verb, static_cast<int>(maxTouched), required, st.SamplerViewStart,
                        st.SamplerViewCount);
                std::abort();
            }
            if (st.SamplerStateStart != 0 || st.SamplerStateCount < required) {
                MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"BindSamplerStates.Count\"} - %s applies with "
                        "units 0..%d touched, so bind_sampler_states must carry Start=0 and Count >= %u "
                        "(CONTRACT-P5E.md §5.3); the applied window is Start=%u Count=%u, which leaves "
                        "an earlier draw's sampler object on at least one unit",
                        verb, static_cast<int>(maxTouched), required, st.SamplerStateStart,
                        st.SamplerStateCount);
                std::abort();
            }
        }
    } // namespace
#endif

    Bool ServerVerbSink::OnDrawVbo(const MG_Pipe::MGPDrawInfo& info,
                                   const MG_Pipe::MGPDrawRange* ranges,
                                   const MG_Pipe::MGHostSpan* userIndices,
                                   const MG_Pipe::MGPDrawIndirect* indirect) {
        if (userIndices != nullptr) {
            Wire::CheckDrawUserIndices(info, ranges, *userIndices);
        }
        // The witness first, before the backend is consulted, so a unit process with no
        // backend object still sees the wire's fields (PipeApplier.h LastDraw).
        m_lastDraw = LastDrawRecord{};
        m_lastDraw.Info = info;
        if (ranges != nullptr && info.NumDraws != 0) m_lastDraw.FirstRange = ranges[0];
        if (userIndices != nullptr) {
            m_lastDraw.HadUserIndices = true;
            m_lastDraw.UserIndexBytes = userIndices->Size;
        }
        if (indirect != nullptr) {
            m_lastDraw.HadIndirect = true;
            m_lastDraw.Indirect = *indirect;
        }
        ++m_drawRecords;

#if MOBILEGL_BUILD_DISAGGREGATED
        // Ruling 19 / ID-95: the window promise, checked at every draw, before the backend is
        // asked to resolve anything out of the windows.
        CheckUnitWindows(MG_Pipe::MGPipeApplier(), "draw_vbo");
#endif
        const MG_Backend::GlobalBackendFunctionsTable* table = Table("draw_vbo");
        if (table == nullptr) return false;
        const MG_Backend::GLFunctionsTable& gl = table->GL;
        const auto mode = static_cast<GLenum>(info.Mode);

        GLenum indexType = 0;
        switch (info.IndexSize) {
        case 0: break; // arrays
        case 1: indexType = GL_UNSIGNED_BYTE; break;
        case 2: indexType = GL_UNSIGNED_SHORT; break;
        case 4: indexType = GL_UNSIGNED_INT; break;
        default:
            // IndexSize is "0 = arrays, else 1 / 2 / 4" (MGPipeTypes.h) and nothing else is a
            // legal width; defaulting to 4 would read past the element buffer.
            Wire::WireProtocolFatalAt("MGPDrawInfo::IndexSize", info.IndexSize, 4);
        }

        // ---- the indirect family: the block is the whole description --------------------
        if (indirect != nullptr) {
            // The layout already refused a record that sets both flags or declares ranges
            // beside the block, so NumDraws is 0 and there is no span here.
#if MOBILEGL_BUILD_DISAGGREGATED
            // P5c (hd, CONTRACT-P5C §3.5): the command/parameter buffer handles cross as the
            // verb's own state; the backend's indirect arm resolves the buffer twins from
            // them instead of reading the client's GL_DRAW_INDIRECT_BUFFER binding slot.
            auto& applierState = MG_Pipe::MGPipeApplier();
            applierState.ClearVerbHandles();
            applierState.VerbIndirectBuffer = indirect->Buffer;
            applierState.VerbIndirectParameterBuffer = indirect->ParameterBuffer;
#endif
            const auto offset = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(indirect->Offset));
            const auto drawCount = static_cast<GLsizei>(indirect->DrawCount);
            const auto stride = static_cast<GLsizei>(indirect->Stride);
            const Bool counted = !MG_Pipe::MGPipeHandleIsNull(indirect->ParameterBuffer);
            const auto parameterOffset = static_cast<GLintptr>(indirect->ParameterOffset);
            // glMultiDraw*Indirect with drawcount 1 and stride 0 IS glDraw*Indirect by GL's own
            // definition, so the single-draw entry point is the one the monolith reaches for
            // the single-draw call and nothing is lost for the multi-draw spelling of it.
            const Bool single = !counted && drawCount == 1 && stride == 0;
            if (info.IndexSize == 0) {
                if (counted) {
                    if (gl.MultiDrawArraysIndirectCount == nullptr) return false;
                    gl.MultiDrawArraysIndirectCount(mode, offset, parameterOffset, drawCount, stride);
                } else if (single) {
                    if (gl.DrawArraysIndirect == nullptr) return false;
                    gl.DrawArraysIndirect(mode, offset);
                } else {
                    if (gl.MultiDrawArraysIndirect == nullptr) return false;
                    gl.MultiDrawArraysIndirect(mode, offset, drawCount, stride);
                }
            } else {
                if (counted) {
                    if (gl.MultiDrawElementsIndirectCount == nullptr) return false;
                    gl.MultiDrawElementsIndirectCount(mode, indexType, offset, parameterOffset,
                                                      drawCount, stride);
                } else if (single) {
                    if (gl.DrawElementsIndirect == nullptr) return false;
                    gl.DrawElementsIndirect(mode, indexType, offset);
                } else {
                    if (gl.MultiDrawElementsIndirect == nullptr) return false;
                    gl.MultiDrawElementsIndirect(mode, indexType, offset, drawCount, stride);
                }
            }
            ++m_draws;
            return true;
        }

        if (ranges == nullptr || info.NumDraws == 0) return false;
        const Bool instanced = info.InstanceCount != 1 || info.StartInstance != 0;
        const auto instanceCount = static_cast<GLsizei>(info.InstanceCount);
        const GLuint baseInstance = info.StartInstance;

        // ---- the multi-draws: the two arrays rebuilt from the ranges (rule C) --------------
        if (info.NumDraws != 1) {
            if (userIndices != nullptr) {
                ServerUnmigratedVerbFatal(info.IndexSize == 0 ? "MultiDrawArrays+CLIENT_INDICES"
                                                              : "MultiDrawElements+CLIENT_INDICES");
            }
            if (instanced) {
                ServerUnmigratedVerbFatal(info.IndexSize == 0 ? "MultiDrawArrays+INSTANCED"
                                                              : "MultiDrawElements+INSTANCED");
            }
            const auto n = static_cast<SizeT>(info.NumDraws);
            m_multiCounts.resize(n);
            if (info.IndexSize == 0) {
                if (gl.MultiDrawArrays == nullptr) return false;
                m_multiFirsts.resize(n);
                for (SizeT i = 0; i < n; ++i) {
                    m_multiFirsts[i] = static_cast<GLint>(ranges[i].Start);
                    m_multiCounts[i] = static_cast<GLsizei>(ranges[i].Count);
                }
                gl.MultiDrawArrays(mode, m_multiFirsts.data(), m_multiCounts.data(),
                                   static_cast<GLsizei>(n));
            } else {
                if (gl.MultiDrawElementsBaseVertex == nullptr) return false;
                m_multiOffsets.resize(n);
                m_multiBaseVertices.resize(n);
                for (SizeT i = 0; i < n; ++i) {
                    m_multiOffsets[i] = reinterpret_cast<const void*>(
                        static_cast<std::uintptr_t>(ranges[i].Start) * info.IndexSize);
                    m_multiCounts[i] = static_cast<GLsizei>(ranges[i].Count);
                    m_multiBaseVertices[i] = ranges[i].IndexBias;
                }
                // MultiDrawElements is the same call with every base vertex 0, which is what
                // its ranges carry (CONTRACT-P5B.md d1's MultiDrawElements row).
                gl.MultiDrawElementsBaseVertex(mode, m_multiCounts.data(), indexType,
                                               m_multiOffsets.data(), static_cast<GLsizei>(n),
                                               m_multiBaseVertices.data());
            }
            ++m_draws;
            return true;
        }

        // ---- one range ----------------------------------------------------------------------
        const MG_Pipe::MGPDrawRange& range = ranges[0];
        const auto count = static_cast<GLsizei>(range.Count);
        if (info.IndexSize == 0) {
            if (!instanced) {
                if (gl.DrawArrays == nullptr) return false;
                gl.DrawArrays(mode, static_cast<GLint>(range.Start), count);
            } else if (baseInstance != 0) {
                if (gl.DrawArraysInstancedBaseInstance == nullptr) return false;
                gl.DrawArraysInstancedBaseInstance(mode, static_cast<GLint>(range.Start), count,
                                                   instanceCount, baseInstance);
            } else {
                if (gl.DrawArraysInstanced == nullptr) return false;
                gl.DrawArraysInstanced(mode, static_cast<GLint>(range.Start), count, instanceCount);
            }
            ++m_draws;
            return true;
        }

        // Indexed. `indices` is the element-buffer byte offset Start * IndexSize - the same
        // arithmetic PipeFill's emitter inverted - or, for a client index array, the staged run
        // resolved through the process resolver the server installed (rule C: the pointer is
        // used for this call only). The codec proved the span lies inside SEG_STAGE (all four
        // honesty arms), so a null here is the resolver missing, which is a wiring fault.
        const void* indices = nullptr;
        if (userIndices != nullptr) {
            indices = MG_Pipe::MGPipeHostBytes(*userIndices);
            if (indices == nullptr) {
                Wire::WireProtocolFatal("DrawVbo.userIndices",
                                        "the user-index span passed the codec's four honesty arms "
                                        "but MGPipeHostBytes resolved it to null; the server's "
                                        "segment resolver is not installed");
            }
        } else {
            indices = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(range.Start) *
                                                    info.IndexSize);
        }
        const GLint baseVertex = range.IndexBias;
        const Bool ranged = (info.Flags & MG_Pipe::kDrawHasIndexRange) != 0;
        // A BASE VERTEX OF 0 IS THE PLAIN ENTRY POINT, NOT THE BaseVertex ONE WITH A 0 - d1's one
        // ruling against CONTRACT-P5B.md's "DrawElementsBaseVertex(..., 0), the P5 arm,
        // unchanged". Espryt's DrawElementsBaseVertex slot calls glDrawElementsBaseVertex, which
        // is ES 3.2 / OES_draw_elements_base_vertex and is NOT loaded on an ES 3.1 provider
        // (ANGLE on D3D11: "Failed to load GLES function: glDrawElementsBaseVertex" at bring-up),
        // so the P5 arm dereferenced a null function pointer on every plain glDrawElements - the
        // one call every Minecraft frame is made of - while the monolith, which calls
        // GL.DrawElements for glDrawElements, was fine. Rule D says the sink reproduces the call
        // the monolith makes; this is that, for all three indexed families.
        if (!instanced) {
            if (ranged) {
                if (baseVertex != 0) {
                    if (gl.DrawRangeElementsBaseVertex == nullptr) return false;
                    gl.DrawRangeElementsBaseVertex(mode, info.MinIndex, info.MaxIndex, count, indexType,
                                                   indices, baseVertex);
                } else {
                    if (gl.DrawRangeElements == nullptr) return false;
                    gl.DrawRangeElements(mode, info.MinIndex, info.MaxIndex, count, indexType, indices);
                }
            } else if (baseVertex != 0) {
                if (gl.DrawElementsBaseVertex == nullptr) return false;
                gl.DrawElementsBaseVertex(mode, count, indexType, indices, baseVertex);
            } else {
                if (gl.DrawElements == nullptr) return false;
                gl.DrawElements(mode, count, indexType, indices);
            }
        } else if (baseInstance != 0) {
            if (baseVertex != 0) {
                if (gl.DrawElementsInstancedBaseVertexBaseInstance == nullptr) return false;
                gl.DrawElementsInstancedBaseVertexBaseInstance(mode, count, indexType, indices,
                                                               instanceCount, baseVertex, baseInstance);
            } else {
                if (gl.DrawElementsInstancedBaseInstance == nullptr) return false;
                gl.DrawElementsInstancedBaseInstance(mode, count, indexType, indices, instanceCount,
                                                     baseInstance);
            }
        } else if (baseVertex != 0) {
            if (gl.DrawElementsInstancedBaseVertex == nullptr) return false;
            gl.DrawElementsInstancedBaseVertex(mode, count, indexType, indices, instanceCount, baseVertex);
        } else {
            if (gl.DrawElementsInstanced == nullptr) return false;
            gl.DrawElementsInstanced(mode, count, indexType, indices, instanceCount);
        }
        ++m_draws;
        return true;
    }

    // -----------------------------------------------------------------------------------
    // P5b: the stubs the four migration packages replace (MG_Remote/CONTRACT-P5B.md).
    //
    // Each dies by the GL slot's own name. The record has crossed and been validated by the
    // codec by the time one of these runs, so the only thing missing is the backend call, and
    // the package that owns the row writes it here: `Table("<row>")`, the null-slot check
    // (a backend that leaves the slot null DECLINES, which is the monolith's null-slot answer
    // in the same words), the call, and a tally the lane can assert moved.
    // -----------------------------------------------------------------------------------

    // ---- i1 ---- (MG_Remote/CONTRACT-P5B.md §2 i1; landed by package p5b/i1)
    //
    // RULE D IN FIVE BODIES. Each reproduces the backend call the monolith makes, from the
    // record and from server state, and NOTHING ELSE: the backend goes on reading the frontend
    // fields it reads today through the BARRIER-PULLED entries of its verb class, which the
    // verb stamp PipeApplier::ApplyOne put up before this sink ran is exactly what makes legal.
    // That is why none of these touches a backend file and why the monolith path is byte
    // identical - and it is also the honest statement of the debt, which `rsp` counts.

    Bool ServerVerbSink::OnLaunchGrid(const MG_Pipe::MGPGridInfo& grid) {
#if MOBILEGL_BUILD_DISAGGREGATED
        // Ruling 19 / ID-95: a dispatch samples through the same unit windows a draw does.
        CheckUnitWindows(MG_Pipe::MGPipeApplier(), "launch_grid");
#endif
        const MG_Backend::GlobalBackendFunctionsTable* table = Table("launch_grid");
        if (table == nullptr) return false;
        const MG_Backend::GLFunctionsTable& gl = table->GL;
        // The compute program is NOT named by THIS record and must not be - but it IS named, by
        // the set_dispatch_program record that preceded it, and the backend's PrepareForCompute
        // reads MGPipeApplier().DispatchProgram from there.
        //
        // P5e (pa): that is what this comment used to get wrong. Until P5e the backend PULLED
        // GetProgramForDispatch - GetProgramForDraw's twin - inside PrepareForCompute, and the
        // field's FATAL -> BARRIER_PULLED move in FieldOwnership.def (contract §6.9, i1's one
        // granted row) is what made the pull legal. pg gave the sync and the link/SPIR-V gate a
        // handle arm and pa retired the pull itself, so on the handle arm the row is not read at
        // all; the BARRIER_PULLED class stays because the monolith arm still reads it (ID-81).
        // Block* are 0 on the wire for the unchanged reason: the local size is a link artifact
        // the backend reads from the program it resolved, which on that arm is the record's
        // archive.
        if (grid.IsIndirect != 0) {
            if (gl.DispatchComputeIndirect == nullptr) return false;
            // IndirectBuffer travels for P7's sake; the BINDING is server state, put there by
            // the set_buffer_bindings record that preceded this one, exactly as OnClear's Fbo
            // is not re-resolved here. glDispatchComputeIndirect takes only the offset.
#if MOBILEGL_BUILD_DISAGGREGATED
            // P5c (hd, CONTRACT-P5C §3.5): the buffer handle itself is now also the verb's own
            // state, so the backend's dispatch-indirect arm resolves the twin from the record
            // rather than from the client's GL_DISPATCH_INDIRECT_BUFFER binding slot.
            auto& applierState = MG_Pipe::MGPipeApplier();
            applierState.ClearVerbHandles();
            applierState.VerbDispatchIndirectBuffer = grid.IndirectBuffer;
#endif
            gl.DispatchComputeIndirect(static_cast<GLintptr>(grid.IndirectOffset));
        } else {
            if (gl.DispatchCompute == nullptr) return false;
            gl.DispatchCompute(static_cast<GLuint>(grid.GridX), static_cast<GLuint>(grid.GridY),
                               static_cast<GLuint>(grid.GridZ));
        }
        ++m_dispatches;
        return true;
    }

    Bool ServerVerbSink::OnMemoryBarrier(const MG_Pipe::MGPMemoryBarrier& barrier) {
        const MG_Backend::GlobalBackendFunctionsTable* table = Table("memory_barrier");
        if (table == nullptr) return false;
        const MG_Backend::GLFunctionsTable& gl = table->GL;
        // THE BITS GO OVER VERBATIM AND ARE LOWERED HERE BY NOBODY. Espryt's atomic-counter
        // lowering - the counter bit implying the storage bit, because glslang lowers every
        // atomic_uint onto a storage block - lives inside its own MemoryBarrier
        // (DirectGLES.cpp:8837) and is a statement about the DRIVER. Repeating it on this side
        // would make the split arm and the monolith arm two different calls.
        if (barrier.ByRegion != 0) {
            if (gl.MemoryBarrierByRegion == nullptr) return false;
            gl.MemoryBarrierByRegion(static_cast<GLbitfield>(barrier.Bits));
        } else {
            if (gl.MemoryBarrier == nullptr) return false;
            gl.MemoryBarrier(static_cast<GLbitfield>(barrier.Bits));
        }
        ++m_memoryBarriers;
        return true;
    }

    Bool ServerVerbSink::OnResourceCopyRegion(const MG_Pipe::MGPCopyRegion& copy) {
        const MG_Backend::GlobalBackendFunctionsTable* table = Table("resource_copy_region");
        if (table == nullptr) return false;
        if (table->GL.CopyImageSubData == nullptr) return false;

        // REFUSED BY NAME ON BOTH SIDES OF ONE WIRE. The client refuses a renderbuffer endpoint
        // before it emits (ID-57's shape, EmitTables.cpp), and this is the same refusal for a
        // record that reached here anyway: no sticky forward hands out a RenderbufferObject, so
        // there is no honest way to build the endpoint, and guessing an empty one would copy
        // nothing and say it copied.

        // P5f fe: both texture identities already travel in the record. Pass those handles
        // to the backend's server-owned texture table; GL names are diagnostic data only.
        MG_Backend::CopyImageEndpoint src{};
        MG_Backend::CopyImageEndpoint dst{};
        if (copy.SrcTarget == GL_RENDERBUFFER) src.RenderbufferHandle = copy.Src;
        else src.TextureHandle = copy.Src;
        if (copy.DstTarget == GL_RENDERBUFFER) dst.RenderbufferHandle = copy.Dst;
        else dst.TextureHandle = copy.Dst;
        if (MG_Pipe::MGPipeHandleIsNull(copy.Src) || MG_Pipe::MGPipeHandleIsNull(copy.Dst)) {
            // The monolith's own answer to this, in its own words (DirectGLES.cpp:9067
            // "source or destination image failed to sync; declining the copy"): the frontend
            // validator is what keeps it unreachable and what reports the INVALID_VALUE the
            // application is owed. A decline here is a real answer, not a silent success.
            MGLOG_E_ONCE("MG_Remote server: resource_copy_region named texture(s) %u -> %u that "
                         "the frontend no longer holds; declining the copy",
                         static_cast<unsigned>(copy.SrcGlName),
                         static_cast<unsigned>(copy.DstGlName));
            return false;
        }

        // SrcBox is {origin, extent} and the extent is the copy's, spelled once by GL for both
        // endpoints; the destination contributes only its origin.
        table->GL.CopyImageSubData(src, static_cast<GLenum>(copy.SrcTarget),
                                   static_cast<GLint>(copy.SrcLevel), copy.SrcBox.X, copy.SrcBox.Y,
                                   copy.SrcBox.Z, dst, static_cast<GLenum>(copy.DstTarget),
                                   static_cast<GLint>(copy.DstLevel), copy.DstX, copy.DstY,
                                   copy.DstZ, static_cast<GLsizei>(copy.SrcBox.W),
                                   static_cast<GLsizei>(copy.SrcBox.H),
                                   static_cast<GLsizei>(copy.SrcBox.D));
        ++m_imageCopies;
        return true;
    }

    Bool ServerVerbSink::OnBindShaderImage(const MG_Pipe::MGPImageBind& bind) {
        const MG_Backend::GlobalBackendFunctionsTable* table = Table("bind_shader_image");
        if (table == nullptr) return false;
        if (table->GL.BindImageTexture == nullptr) return false;
        // THE SAME CALL IS RIGHT FOR BOTH BACKENDS, which is why the record carries the whole
        // argument list although neither reads all of it today: Espryt ignores everything but
        // Unit and syncs that unit from the barrier-pulled GetImageTextureBinding
        // (DirectGLES.cpp:9154, :2471), and Magma's slot is a no-op (DirectVulkan.cpp:665). The
        // arguments travel because rule D says a verb crosses as the CALL, and because P7 is
        // what makes the backend read them instead of pulling.
        table->GL.BindImageTexture(static_cast<GLuint>(bind.Unit), static_cast<GLuint>(bind.GlName),
                                   static_cast<GLint>(bind.Level),
                                   bind.Layered != 0 ? GL_TRUE : GL_FALSE,
                                   static_cast<GLint>(bind.Layer),
                                   static_cast<GLenum>(bind.Access),
                                   static_cast<GLenum>(bind.Format));
        ++m_imageBinds;
        // P5c (rv): an image bind moves the frontend's texture bind generation, and this verb
        // carries no set_shader_images alongside it - so the server-side shutter serial the
        // accessors now answer with (CONTRACT-P5C.md §5.3) moves here, at the one place the
        // event reaches the applier's side.
        MG_Pipe::MGPipeApplierNoteTextureStateMoved();
        return true;
    }

    Bool ServerVerbSink::OnSetStorageBlockBinding(const MG_Pipe::MGPStorageBlockBinding& binding,
                                                  const char* name) {
        const MG_Backend::GlobalBackendFunctionsTable* table = Table("set_storage_block_binding");
        if (table == nullptr) return false;
        if (table->GL.ShaderStorageBlockBinding == nullptr) return false;
        if (name == nullptr) return false;
        // The NAME is the one coordinate the application, the frontend and both backends agree
        // on (BackendObject.h:216-221), which is why the row carries a blob rather than the
        // application's block INDEX. `name` points into the decoder's bounded local and is
        // valid for this call only (rule C); the backend slot copies what it needs.
        //
        // P5f fe: publish the record's program handle for the backend consumer. Espryt
        // resolves its server twin directly; Magma's consumer is the fm package's seam.
        MG_Pipe::MGPipeApplier().ClearVerbHandles();
        MG_Pipe::MGPipeApplier().VerbStorageBlockProgram = binding.ShaderCso;
        table->GL.ShaderStorageBlockBinding(static_cast<GLuint>(binding.GlName), name,
                                            static_cast<GLuint>(binding.Binding));
        ++m_storageBlockBindings;
        return true;
    }

    // ---- t2 ---- (MG_Remote/CONTRACT-P5B.md §2 t2)
    //
    // SIX BODIES, SIX BACKEND CALLS, NO STATE OF THEIR OWN. Rule D: the record IS the call, and
    // everything the backend reads around it - the capture program, the capture-buffer
    // bindings, the bound XFB object, the patch state - it reads from gPipeInputs through its
    // verb class's BARRIER-PULLED fields, which the client filled at the call site and the
    // verb barrier holds still (R-1). That is why none of these touches m_backend beyond
    // Table() and why not one of them caches anything across records.
    //
    // A NULL SLOT DECLINES, AND THE DECLINE IS THE MONOLITH'S ANSWER IN THE SAME WORDS. Magma
    // (DirectVulkan) registers NO XFB slot and no PatchParameteri at all
    // (BackendObject_DirectVulkan.cpp), and under monolith the frontend's own
    // `if (const auto f = table.GL.X)` guard simply skips the call; `return false` here is that
    // same skip, reported to DecodeAndApply as "this build did not apply it" rather than as a
    // crash or as a silent success. Contract §2 t2 says so for PatchParameteri by name.

    Bool ServerVerbSink::OnBeginStreamOutput(const MG_Pipe::MGPStreamOutputBegin& begin) {
        auto& state = MG_Pipe::MGPipeApplier();
        state.BoundStreamOutputLifetimeId = begin.LifetimeId;
        state.StreamOutputSpans[begin.LifetimeId] = begin;
        const MG_Backend::GlobalBackendFunctionsTable* table = Table("begin_stream_output");
        if (table == nullptr) return false;
        if (table->GL.BeginTransformFeedback == nullptr) return false;
        // Espryt's Begin only ARMS the span (DirectGLES.cpp:1212-1220: primitiveMode, pending,
        // targets cleared); the driver glBeginTransformFeedback happens in the tail of the next
        // PrepareForDraw (StartPendingTransformFeedback, :1224), where the capture program and
        // the buffer bindings are read through the pulls. So this record's effect is not
        // visible until a DRAW crosses - which is why an XFB scenario whose draw is still class
        // C moves its first blocker to that draw rather than rendering.
        table->GL.BeginTransformFeedback(static_cast<GLenum>(begin.PrimitiveMode));
        ++m_streamOutputSpans;
        return true;
    }

    Bool ServerVerbSink::OnEndStreamOutput(const MG_Pipe::MGPXfbAccounting& accounting) {
        const MG_Backend::GlobalBackendFunctionsTable* table = Table("end_stream_output");
        if (table == nullptr) return false;
        if (table->GL.EndTransformFeedback == nullptr) {
            auto& state = MG_Pipe::MGPipeApplier();
            state.StreamOutputSpans.erase(state.BoundStreamOutputLifetimeId);
            return false;
        }
        // THE THREE ACCOUNTING FIELDS ARE NOT READ, AND THAT IS THE RULING RATHER THAN AN
        // OMISSION. glEndTransformFeedback takes no arguments; the numbers are the CLIENT's own
        // per-span accounting (contract §2 t2's companions row) and the client is where they are
        // consumed - by the primitive queries and by the capture-capacity clamp. A server that
        // second-guessed them from its own driver would be publishing a second answer to a
        // question the frontend already answers, and the second answer is the one that goes
        // stale. They cross because the row has carried them since P4a and because P9's
        // server-side scatter is what will need them.
        (void)accounting;
        table->GL.EndTransformFeedback();
        auto& state = MG_Pipe::MGPipeApplier();
        state.StreamOutputSpans.erase(state.BoundStreamOutputLifetimeId);
        ++m_streamOutputSpans;
        return true;
    }

    Bool ServerVerbSink::OnPauseStreamOutput(const MG_Pipe::MGPStreamOutputControl& control) {
        const MG_Backend::GlobalBackendFunctionsTable* table = Table("pause_stream_output");
        if (table == nullptr) return false;
        if (table->GL.PauseTransformFeedback == nullptr) return false;
        (void)control; // Reserved, and the contract says it is 0.
        table->GL.PauseTransformFeedback();
        ++m_streamOutputControls;
        return true;
    }

    Bool ServerVerbSink::OnResumeStreamOutput(const MG_Pipe::MGPStreamOutputControl& control) {
        const MG_Backend::GlobalBackendFunctionsTable* table = Table("resume_stream_output");
        if (table == nullptr) return false;
        if (table->GL.ResumeTransformFeedback == nullptr) return false;
        (void)control;
        table->GL.ResumeTransformFeedback();
        ++m_streamOutputControls;
        return true;
    }

    Bool ServerVerbSink::OnBindStreamOutput(const MG_Pipe::MGPStreamOutputBind& bind) {
        const MG_Backend::GlobalBackendFunctionsTable* table = Table("bind_stream_output");
        if (table == nullptr) return false;
        // THE GL NAME IS THE ARGUMENT, NOT THE LifetimeId BESIDE IT. Espryt keys its driver
        // objects by the GL name (XfbImpl::g_xfbObjects[name], DirectGLES.cpp:1401) and creates
        // the ES object on first bind; passing the lifetime id would index a map that has never
        // heard of it and silently create a second driver object per bind. The lifetime id
        // identifies the P5f capture snapshot on this side. Keeping it separate preserves
        // the backend GL-name argument while Begin/End find the right server-owned span.
        MG_Pipe::MGPipeApplier().BoundStreamOutputLifetimeId = bind.LifetimeId;
        if (table->GL.BindTransformFeedback) table->GL.BindTransformFeedback(static_cast<GLuint>(bind.GlName));
        ++m_streamOutputBinds;
        return true;
    }

    Bool ServerVerbSink::OnDeleteStreamOutput(const MG_Pipe::MGPStreamOutputBind& object) {
        const auto* table = Table("DeleteTransformFeedback");
        if (!table) return false;
        if (!object.GlName || object.Pad0)
            Wire::WireProtocolFatal("DeleteStreamOutput.name", "default object or reserved field");
        auto& state = MG_Pipe::MGPipeApplier();
        if (object.LifetimeId) {
            state.StreamOutputSpans.erase(object.LifetimeId);
            if (state.BoundStreamOutputLifetimeId == object.LifetimeId) state.BoundStreamOutputLifetimeId = 0;
        }
        if (table->GL.DeleteTransformFeedback) table->GL.DeleteTransformFeedback(object.GlName);
        return true;
    }

    Bool ServerVerbSink::OnPatchParameter(const MG_Pipe::MGPPatchParameter& patch) {
        const MG_Backend::GlobalBackendFunctionsTable* table = Table("patch_parameter");
        if (table == nullptr) return false;
        // Magma registers no PatchParameteri: it compiles the patch size into its synthesized
        // control stage from set_patch_state instead, so the DECLINE below is the whole of the
        // right answer for that backend and not a gap (contract §2 t2).
        if (table->GL.PatchParameteri == nullptr) return false;
        // Pname is GL_PATCH_VERTICES and the frontend has already rejected every other spelling
        // with INVALID_ENUM before the record was built, so this is a forward and not a switch.
        table->GL.PatchParameteri(static_cast<GLenum>(patch.Pname), static_cast<GLint>(patch.Value));
        ++m_patchParameters;
        return true;
    }

    // ---- f1 ----
    Bool ServerVerbSink::OnGenerateMipmap(const MG_Pipe::MGPMipPlan& plan) {
        const auto* table = Table("GenerateMipmap");
        if (table == nullptr || table->GL.GenerateMipmap == nullptr) return false;
#if MOBILEGL_BUILD_DISAGGREGATED
        // P5c (hd, CONTRACT-P5C §3.2): the texture the client resolved at Target on the active
        // unit crosses as the verb's own state; the backend's mip-descriptor check resolves
        // the record from it instead of probing the client allocator for the bound object's
        // lifetime id (T2's mip half).
        auto& applierState = MG_Pipe::MGPipeApplier();
        applierState.ClearVerbHandles();
        applierState.VerbMipRes = plan.Res;
        applierState.VerbMipBaseLevel = plan.BaseLevel;
        applierState.VerbMipLevelCount = plan.LevelCount;
#endif
        table->GL.GenerateMipmap(plan.Target);
        return true;
    }

    Bool ServerVerbSink::OnCopyFramebufferToTexture(const MG_Pipe::MGPCopyFromFramebuffer& copy) {
        const auto* table = Table(copy.SubImage ? "CopyTexSubImage2D" : "CopyTexImage2D");
        if (table == nullptr) return false;
#if MOBILEGL_BUILD_DISAGGREGATED
        // P5c (hd, CONTRACT-P5C §3.4): the destination texture the client resolved at the
        // active unit crosses as the verb's own state; the backend resolves its twin from the
        // handle instead of reading the client's texture-unit binding slot (T4).
        auto& applierState = MG_Pipe::MGPipeApplier();
        applierState.ClearVerbHandles();
        applierState.VerbCopyTexDst = copy.Dst;
#endif
        if (copy.SubImage) {
            if (table->GL.CopyTexSubImage2D == nullptr) return false;
            table->GL.CopyTexSubImage2D(copy.Target, copy.Level, copy.XOffset, copy.YOffset,
                                      copy.X, copy.Y, copy.Width, copy.Height);
        } else {
            if (table->GL.CopyTexImage2D == nullptr) return false;
            table->GL.CopyTexImage2D(copy.Target, copy.Level, copy.InternalFormat,
                                   copy.X, copy.Y, copy.Width, copy.Height, 0);
        }
        return true;
    }

    // ---- P5c ct (MG_Remote/CONTRACT-P5C.md §5) ------------------------------------------
    //
    // TWO CONTROL RECORDS, NO BACKEND TABLE AND NO DECLINE ARM. Neither body consults
    // Table(): the reset belongs to the applier this process owns, and the death release
    // belongs to the twin tables - a backend that registered no slots (Magma's XFB shape)
    // still has an applier to reset and still answers a death with the same generation
    // check. A record that cannot be proved is Fatal, not declined: both refusals are
    // ProtocolCorruption because by the time the sink runs the codec has already proved the
    // record's SHAPE, and what is left to check are the contract facts about the peer (§1).

    Bool ServerVerbSink::OnApplierReset(const MG_Pipe::MGPApplierReset& reset) {
        // §1: the serial is ASSERTED, never dispatched on. P5c has exactly one context per
        // session, so the only legal sequence is 0, 1, 2, ... and the session's own count of
        // accepted resets IS the expected value; anything else means the two ends disagree
        // about how many make-current edges have crossed, which no backend answer can fix.
        if (reset.ContextSerial != m_applierResetSerial) {
            MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"ApplierReset.ContextSerial\"} - the "
                    "record carries %llu and this session has accepted %llu reset(s); the "
                    "serial is asserted against the session's own count, not dispatched on "
                    "(one context per session in P5c)",
                    static_cast<unsigned long long>(reset.ContextSerial),
                    static_cast<unsigned long long>(m_applierResetSerial));
            std::abort();
        }
        ++m_applierResetSerial;
        // THE WHOLE POINT OF THE RECORD: the reset runs HERE, on the apply thread, against
        // the g_applier this role owns (PipeApply.cpp:409). The layer-2 guard inside
        // MGPipeApplierReset passes because this IS the apply thread; the GL-thread direct
        // call it replaced is the Fatal arm.
        MG_Pipe::MGPipeApplierReset();
        ++m_applierResets;
        return true;
    }

    Bool ServerVerbSink::OnObjectDeath(const MG_Pipe::MGPHandleOnly& death) {
        // §1's zero ruling: a null handle means "the object never crossed", and the client
        // emits NOTHING in that case (§5.2) - so a null handle arriving here is corruption,
        // not a no-op.
        if (MG_Pipe::MGPipeHandleIsNull(death.Handle)) {
            MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"ObjectDeath.Handle\"} - a null "
                    "handle never crosses: the client emits nothing for an object its own "
                    "allocator cannot resolve (CONTRACT-P5C.md §5.2)");
            std::abort();
        }
        if (death.Kind >= static_cast<Uint32>(MG_Pipe::MGPipeKind::KindCount)) {
            MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"ObjectDeath.Kind\"} - %u is not an "
                    "MGPipeKind",
                    static_cast<unsigned>(death.Kind));
            std::abort();
        }
        // The per-kind release, keyed by the handle the record carried. A false answer is
        // NOT a decline: the kind's own delete opcode may already have released the twin
        // (the idempotent second path every notice arm documents), and a kind this backend
        // does not twin (Buffer, whose death crosses as resource_destroy) legally resolves
        // to nothing.
        MG_Backend::DirectGLES::ReleaseTwinsForWireObjectDeath(
            death.Handle, static_cast<MG_Pipe::MGPipeKind>(death.Kind));
        ++m_objectDeaths;
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
        // P5f (f1), CONTRACT-P5E §3.2: the server block's identity, once per session (the
        // per-verb stamp refreshes it against the served-context serial). A no-op unless the
        // dual-block rehearsal is armed; without it the server block's ContextIdentity() stays
        // nullptr and the backend's identity-keyed memo caches read that as a HIT on their
        // zero-initialised slot - an unnamed null dereference instead of a named marker.
        MG_Pipe::MGPipeServerBlockNoteIdentity();
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
        // P5e (id), CONTRACT-P5E §2.1 / §4.4: STAMP WHETHER THE CLIENT IS PARKED BEHIND THIS
        // RECORD, before anything can ask. It is the input to the allocator guard's exemption
        // (SlotAllocator.cpp) and to every frontend-keyed twin member that survives as monolith
        // glue (SlotTables.h), and it has to be up before DecodeAndApply because the sinks those
        // reach are exactly the askers. MGPipeBarriered answers true for every record until ra
        // lands the wait rule, so this line changes nothing this phase and is the line ra
        // rebases onto rather than adds.
        //
        // AND THE STAMP IS `true` UNTIL ra LANDS THE CLIENT'S HALF (ID-103). The predicate
        // describes what the client WILL do once EmitAndWaitTails follows the wait classes;
        // today it still blocks after every record, so a `false` stamp here would withdraw the
        // §4.4 exemptions from a probe the client's own wait still makes safe - a refusal with
        // no defect behind it. The predicate is computed on every record all the same, so it is
        // exercised for the whole phase rather than first run on the day it starts deciding.
        //
        // AND THE SERVER'S OWN CAPABILITY IS THE SECOND CONJUNCT (P5e gl, ID-111). The constant
        // above says what the CLIENT will do once ra's wait rule is live; it says nothing about
        // which server this is. kCapRunAheadApply is never published on DirectVulkan (ID-90,
        // MGPipeRunAheadCapBitsFor), yet draw_vbo / blit / clear / launch_grid are all
        // kWaitNone - so a stamp that read the build constant alone would, on the day it flips,
        // have a MAGMA server mark every draw record UNBARRIERED while its client is still
        // lockstep. CountBarrierPull (PipeInputs.cpp) is an unconditional Fatal on an
        // unbarriered pull, no knob involved, and Magma's residual fill is its ONLY source for
        // the seven pointer-backed fields: Magma would die on its first draw and take
        // MagmaP7AllocatorDebtScope's exemption with it.
        //
        // BOTH PREDICATES ARE COMPUTED UNCONDITIONALLY, as arguments rather than as the arms of
        // a short-circuit, which is ID-103's reason extended to the capability probe: they are
        // exercised on every record for the whole phase rather than first running on the day
        // they start deciding.
        const Bool wireSaysBarriered = MG_Pipe::MGPipeBarriered(
            static_cast<MG_Pipe::MGPWireOp>(record.kind), record.payload, MG_Pipe::MGPipeApplier());
        MG_Pipe::MGPipeApplierSetCurrentRecordBarriered(
            MGPipeApplierStampsBarriered(MG_Pipe::kMGPipeP5eClientWaitRuleLanded,
                                         MGPipeServerPublishesRunAhead(), wireSaysBarriered));
        // P5e (gl), ID-128: AND WHETHER IT WAS BARRIERED BY ESCALATION RATHER THAN BY ITS CLASS.
        // The predicate above is the static wait class plus two payload-derived escalations
        // (ID-83: an open transform-feedback span, a draw carrying client vertex arrays), so the
        // difference between it and the table IS the escalation - both halves are already
        // computed here, and the second flag costs one compare. The strict knob is the only
        // reader: a pull on an escalated record is a debt the phase that owns XFB (§5.7) or
        // client arrays (ID-82) owes, neither of which is this one.
        MG_Pipe::MGPipeApplierSetCurrentRecordBarrieredByEscalation(
            wireSaysBarriered &&
            MG_Pipe::MGPipeWaitClassFor(static_cast<MG_Pipe::MGPWireOp>(record.kind)) ==
                MG_Pipe::kWaitNone);
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
