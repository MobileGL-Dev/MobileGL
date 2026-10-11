// MobileGL - MobileGL/MG_Remote/Server/PipeApplier.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P5 package v1: the applier bridge, and the consumer for contract 7's five class-B verbs.

#include "PipeApplier.h"
#include <MG_Remote/FatalFunnel.h>

#include "ServerSession.h"
#include "ServerLoop.h"
#include "../Transport/ReplySlot.h"
#include <MG_Backend/Record/StagedTextureStore.h>

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
#if !defined(_WIN32)
#include <unistd.h>
#endif
#include <cstring>
#include <limits>

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
        if (m_link) {
            const auto result = m_link->PostReply(seq, status, bytes, size);
            if (result == MOBILEGL_OK) {
                ++m_posted;

                return;
            }
            ++m_failed;
            if (result == MOBILEGL_ERR_BUFFER_TOO_SMALL)
                SessionFail(MGFatalFamily::ReplyTooLarge, "MGPipe: Fatal{ReplyTooLarge, stream reply}");
            // EVERY OTHER FAILURE IS NAMED AND COUNTED RATHER THAN SWALLOWED (P12). This used to
            // `return` on anything that was not BUFFER_TOO_SMALL, so a TRANSPORT_CLOSED reply
            // vanished with no line, no counter and no Fatal - and the client's only symptom is a
            // deferred answer that never arrives, which is indistinguishable from a buffer that
            // overwrote it. Once per call site rather than once per reply: a broken link fails
            // every time, and a log per answer would drown the run it is there to explain.
            MGLOG_E_ONCE("MGPipe: reply seq %llu was NOT delivered (%d); the client will not read"
                         " this answer. FailedReplies() counts them",
                         static_cast<unsigned long long>(seq), static_cast<int>(result));
            return;
        }
        Transport::ReplySlotPool pool(m_base, m_size, m_slots);
        // Fatal inside Post when the answer does not fit a slot: P5 does not chunk replies,
        // and the client knows an answer's size before it emits the record.
        pool.Post(seq, status, bytes, size);
        ++m_posted;
    }

    Uint32 ReplyPool::SlotBytes() const { return m_slotBytes; }

    // -----------------------------------------------------------------------------------
    // ServerVerbSink - the five class-B verbs
    // -----------------------------------------------------------------------------------

    void ServerVerbSink::OnBackendChanged() {
        ReleaseQueries();
        ReleaseFences();
    }

    ServerVerbSink::FenceEntry& ServerVerbSink::FindFence(MG_Pipe::MGPipeHandle handle) {
        const auto it = m_fences.find(handle.Slot);
        if (handle.Slot == 0 || it == m_fences.end() ||
            !it->second.Live || it->second.Gen != handle.Gen) {
            Wire::WireProtocolFatal("Fence.handle", "missing, destroyed or stale fence handle");
        }
        return it->second;
    }

    Bool ServerVerbSink::OnMapPersistent(const MG_Pipe::MGPHandleOnly& handle, Uint64 seq, Int32& status,
                                         Uint64& inProcessBase) {
        ServerSession* session = ServerSession::Active();
        return session != nullptr && session->AdoptStoreT0(handle, seq, status, inProcessBase);
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
        // P10: in creation order, for ReportSignaledFences - the order the GPU finishes them in.
        m_unreportedFences.push_back(handle);
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
        m_unreportedFences.clear();
    }

    // P10 (CONTRACT-P10.md §1): WHICH OF THE CLIENT'S FENCES HAS THE GPU FINISHED.
    //
    // The client answers a poll of a fence it knows is signaled without asking - that is the
    // whole of package A - and it learns what is signaled only from here. Walked in CREATION
    // order and stopped at the first one still pending: one context's GPU work completes in
    // the order it was submitted, so a later fence cannot be done while an earlier one is not,
    // and the steady-state cost is one status query per drain batch. (Were a backend ever to
    // finish them out of order, the later fence would only be reported late - an answer the
    // client then gets by its own round trip - never early.)
    //
    // `flush`: the apply thread is about to go idle. A fence whose commands the driver still
    // holds never signals on its own, and an idle server submits nothing more, so the first
    // pending fence is asked through ClientWaitSync(GL_SYNC_FLUSH_COMMANDS_BIT, 0) - the one
    // backend-neutral way both backends take to submit what they hold - instead of the plain
    // status query. Only when idle: mid-stream the next records flush soon enough, and a
    // submit per batch would cost Magma its command-buffer batching.
    Uint32 ServerVerbSink::ReportSignaledFences(Bool flush) {
        if (m_unreportedFences.empty()) return 0;
        ServerSession* session = ServerSession::Active();
        if (session == nullptr || m_backend == nullptr) return 0;
        const MG_Backend::GLFunctionsTable& gl = m_backend->GetBackendFunctions().GL;
        Uint32 reported = 0;
        while (!m_unreportedFences.empty()) {
            const MG_Pipe::MGPipeHandle handle = m_unreportedFences.front();
            const auto it = m_fences.find(handle.Slot);
            if (it == m_fences.end() || !it->second.Live || it->second.Gen != handle.Gen) {
                // Deleted before it was seen signaled: nobody can poll it any more.
                m_unreportedFences.pop_front();
                continue;
            }
            const FenceEntry& entry = it->second;
            Bool signaled = entry.Native == nullptr;
            if (!signaled) {
                if (flush && gl.ClientWaitSync != nullptr) {
                    const GLenum status = gl.ClientWaitSync(entry.Native, GL_SYNC_FLUSH_COMMANDS_BIT, 0);
                    signaled = status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED;
                    flush = false; // one submit is all an idle server owes
                    ++m_fenceIdleFlushes;
                } else {
                    signaled = gl.GetSyncStatus == nullptr || gl.GetSyncStatus(entry.Native);
                }
            }
            if (!signaled) break;
            m_unreportedFences.pop_front();
            session->PostFenceSignaled(handle);
            ++reported;
        }
        m_fencesReported += reported;
        return reported;
    }

#include "QueryServer.inc"

    namespace {
        // A record's damage rectangles as a region; 0 rectangles is the whole surface. A count past
        // what the record holds is clamped (the rectangles it names are all there are).
        MG_Util::Damage::Region DamageOfRecord(Uint32 count, const Int32* rects) {
            const Int32 n = static_cast<Int32>(std::min<Uint32>(count, MG_Pipe::kMGPMaxDamageRects));
            return MG_Util::Damage::Region::FromEglRects(rects, n);
        }
    } // namespace

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
        // The frame's damage rides to the native present for the length of this call.
        MG_Backend::SetCurrentPresentDamage(DamageOfRecord(present.DamageCount, present.Damage));
        table->Present();
        MG_Backend::SetCurrentPresentDamage(MG_Util::Damage::Region::Full());
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
            // PH-1 (3): latches in an armed session child - no credit is returned for it, the
            // session closes and the peer's credit wait ends on the hang-up.
            return SessionLatch(MGFatalFamily::ProtocolCorruption, "MGPipe: Fatal{ProtocolCorruption, \"Present.FrameSerial\"} - a run-ahead "
                    "server was handed present serial 0. The client mints this 1-based and "
                    "waits on it for its credit (CONTRACT-P5E §2.4); returning a credit for "
                    "serial 0 would release a wait that is asking about frame N");
        }
        m_lastPresentSerial = present.FrameSerial != 0 ? present.FrameSerial : m_presents;
        // §2.4's other half, and the reason ServerSession::ReturnPresentCredit has had no
        // production caller since v1 wrote it: ONE CREDIT PER SWAP, returned after Present()
        // has returned rather than before it, because what the client is waiting for is the
        // swap and not the record's apply. It advances presentAckSerial AND rings the client's
        // bell - a client parked in WaitForPresentAck(kWaitForever) needs the pair.
        if (ServerSession* session = ServerSession::Active()) {
            // P10 (CONTRACT-P10.md §1): the fences finished by now are reported BEFORE the credit
            // goes back - a client released by the credit polls or waits on the fence of a frame
            // or two ago at once, and a report posted only after this batch would race it. A
            // status query, no submit: the swap has just submitted.
            if (session->EventRingHasRoom()) ReportSignaledFences(/*flush=*/false);
            session->ReturnPresentCredit(m_lastPresentSerial);
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
        // PH-1 (3): latches in an armed session child (the peer wrote the serial); dies unarmed.
        if (reset.ContextSerial != m_applierResetSerial) {
            return SessionLatch(MGFatalFamily::ProtocolCorruption, "MGPipe: Fatal{ProtocolCorruption, \"ApplierReset.ContextSerial\"} - the "
                    "record carries %llu and this session has accepted %llu reset(s); the "
                    "serial is asserted against the session's own count, not dispatched on "
                    "(one context per session in P5c)",
                    static_cast<unsigned long long>(reset.ContextSerial),
                    static_cast<unsigned long long>(m_applierResetSerial));
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

    ServerVerbSink::OwnedFd& ServerVerbSink::OwnedFd::operator=(OwnedFd&& other) noexcept {
        if (this != &other) Reset(std::exchange(other.fd, -1));
        return *this;
    }

    ServerVerbSink::OwnedFd::~OwnedFd() { Reset(-1); }

    void ServerVerbSink::OwnedFd::Reset(int next) {
#if !defined(_WIN32)
        if (fd >= 0) ::close(fd);
#endif
        fd = next;
    }

    // P14 S1. THE RING'S DOOR INTO THE SESSION'S CONTEXT TABLE.
    //
    // The record carries the client's current context token and this is where a session's
    // attribution moves. It is deliberately NOT an assertion like OnApplierReset's serial: the
    // client's EGL layer mints the token and the server has no second count to compare against,
    // so what is checked is a FACT ABOUT THIS SESSION'S OWN TABLE - a token that CreateContext
    // never created is corruption, not a late record. Token 0 is the release edge and is always
    // legal (a session whose client released its context has no context, which is exactly the
    // value a session that never bound one already answers).
    Bool ServerVerbSink::OnSharedImage(const MG_Pipe::MGPSharedImageOp& op, Uint64 seq,
                                       MG_Pipe::MGPSharedImageReply& reply) {
        namespace SI = SharedImages;
        ServerSession* session = ServerSession::Active();
        if (session == nullptr) return false;
        SI::SessionHolder& holder = session->SharedImageHolder();
        // Shared images carry memory between processes. A client in this process has none to
        // carry, so it gets none - no platform buffer is ever allocated or imported for it.
        const Bool peerIsThisProcess = Transport::PeerIsThisProcess(session->ControlTransport());
        const auto fill = [&reply](const SI::Image& image) {
            reply.ImageId = image.Id;
            reply.Modifier = image.Modifier;
            reply.Width = image.Width;
            reply.Height = image.Height;
            reply.Format = image.Fourcc;
            reply.Stride = image.Stride;
            reply.Offset = image.Offset;
            reply.Plane1Stride = image.Plane1Stride;
            reply.Plane1Offset = image.Plane1Offset;
        };
        switch (op.Op) {
        case MG_Pipe::kMGPSharedImageAllocate: {
            if (peerIsThisProcess) {
                MGLOG_W_ONCE("MG_Remote server: a shared-image allocation was refused: the client is this process");
                return false;
            }
            std::string why;
            SI::ImageRef image = SI::Allocate(op.Width, op.Height, op.Format, why);
            if (image == nullptr) {
                MGLOG_E("MG_Remote server: shared image %ux%u (fourcc 0x%08x) not allocated: %s", op.Width, op.Height,
                        op.Format, why.c_str());
                return false;
            }
            Transport::ITransport* transport = session->ControlTransport();
            // The descriptor goes out BEFORE the reply is posted, so the client finds it queued.
            MG_Pipe::MGPSharedImageFdOffer offer{MG_Pipe::kMGPSharedImageFdMagic, 1, seq, image->Id, 0};
            const MobileGLResult shared = transport != nullptr
                                              ? transport->ShareFd(image->Fd, MobileGLByteSpan{&offer, sizeof(offer)})
                                              : MOBILEGL_ERR_NOT_INITIALIZED;
            if (shared != MOBILEGL_OK) {
                MGLOG_E("MG_Remote server: shared image %llu could not be sent on the aux socket (rc=%d)",
                        static_cast<unsigned long long>(image->Id), static_cast<int>(shared));
                return false;
            }
            holder.Hold(image);
            fill(*image);
            MGLOG_D("MG_Remote server: shared image %llu allocated, %ux%u stride %u",
                    static_cast<unsigned long long>(image->Id), image->Width, image->Height, image->Stride);
            return true;
        }
        case MG_Pipe::kMGPSharedImageImport: {
            int fd = -1;
            std::string why;
            if (session->T0Inbox().TakeSharedImageFd(seq, Transport::AdoptT0::kOfferWaitMs, &fd, why) !=
                AdoptInbox::Outcome::Taken) {
                MGLOG_E("MG_Remote server: shared-image import (record %llu) has no descriptor: %s",
                        static_cast<unsigned long long>(seq), why.c_str());
                return false;
            }
            if (peerIsThisProcess) {
#if !defined(_WIN32)
                ::close(fd);
#endif
                MGLOG_W_ONCE("MG_Remote server: a dma-buf import was refused: the client is this process");
                return false;
            }
            SI::ImageRef image = SI::Identify(fd, why);
            // YUV: the planes and colour hints ride in Damage (MGPImportPlaneWord). A buffer this
            // server did not allocate is taken too, through the labelled CPU-copy fallback
            // (SharedImageRegistry.h, Image::ForeignSource); RGBA ones never are.
            const Bool yuv = SI::FourccIsYuv(op.Format) && op.DamageCount == MG_Pipe::kMGPImportPlaneWords;
            if (image == nullptr && yuv) {
                SI::PlaneLayout layout;
                layout.Count = static_cast<Uint32>(op.Damage[MG_Pipe::kMGPImportPlaneCount]);
                layout.Offset[0] = static_cast<Uint32>(op.Damage[MG_Pipe::kMGPImportPlane0Offset]);
                layout.Offset[1] = static_cast<Uint32>(op.Damage[MG_Pipe::kMGPImportPlane1Offset]);
                layout.Pitch[0] = static_cast<Uint32>(op.Damage[MG_Pipe::kMGPImportPlane0Pitch]);
                layout.Pitch[1] = static_cast<Uint32>(op.Damage[MG_Pipe::kMGPImportPlane1Pitch]);
                // The copy reads rows: only a buffer said to be linear (or said nothing) is one.
                const Uint64 modifier =
                    static_cast<Uint64>(static_cast<Uint32>(op.Damage[MG_Pipe::kMGPImportModifierLo])) |
                    (static_cast<Uint64>(static_cast<Uint32>(op.Damage[MG_Pipe::kMGPImportModifierHi])) << 32);
                if (op.Damage[MG_Pipe::kMGPImportHasModifier] != 0 && modifier != SI::kModifierLinear &&
                    modifier != SI::kModifierInvalid) {
                    char text[96];
                    std::snprintf(text, sizeof(text), "; a foreign buffer of modifier 0x%016llx is not linear",
                                  static_cast<unsigned long long>(modifier));
                    why += text;
                } else {
                    std::string foreignWhy;
                    image = SI::ImportForeignYuv(fd, op.Width, op.Height, op.Format, layout, foreignWhy);
                    if (image == nullptr) why += "; as a foreign YUV buffer: " + foreignWhy;
                }
            }
#if !defined(_WIN32)
            ::close(fd);
#endif
            if (image == nullptr) {
                MGLOG_W_ONCE("MG_Remote server: a dma-buf import was refused: %s", why.c_str());
                return false;
            }
            if (SI::FourccIsYuv(image->Fourcc) != SI::FourccIsYuv(op.Format) ||
                (SI::FourccIsYuv(op.Format) && image->Fourcc != op.Format)) {
                MGLOG_W("MG_Remote server: a dma-buf import of image %llu (fourcc 0x%08x) as fourcc 0x%08x was refused",
                        static_cast<unsigned long long>(image->Id), image->Fourcc, op.Format);
                return false;
            }
            if (yuv) {
                SI::Yuv::Hints hints;
                hints.ColorSpace = static_cast<Uint32>(op.Damage[MG_Pipe::kMGPImportYuvColorSpace]);
                hints.Range = static_cast<Uint32>(op.Damage[MG_Pipe::kMGPImportYuvRange]);
                hints.SitingX = static_cast<Uint32>(op.Damage[MG_Pipe::kMGPImportYuvSiting]) & 0xffffu;
                hints.SitingY = static_cast<Uint32>(op.Damage[MG_Pipe::kMGPImportYuvSiting]) >> 16;
                SI::SetYuvHints(*image, hints);
            }
            if (image->Width != op.Width || image->Height != op.Height || !SI::FourccSupported(op.Format)) {
                MGLOG_W("MG_Remote server: a dma-buf import of image %llu (%ux%u) as %ux%u fourcc 0x%08x was refused",
                        static_cast<unsigned long long>(image->Id), image->Width, image->Height, op.Width, op.Height,
                        op.Format);
                return false;
            }
            holder.Hold(image);
            fill(*image);
            return true;
        }
        case MG_Pipe::kMGPSharedImageRelease:
            reply.ImageId = op.ImageId;
            return holder.Release(op.ImageId);
        case MG_Pipe::kMGPSharedImagePresent: {
            SI::ImageRef image = holder.Get(op.ImageId);
            MG_Backend::BackendObject* backend = ServerLoopInstance().Backend();
            if (image == nullptr || backend == nullptr) return false;
            MG_Backend::SharedImageView view;
            view.Id = image->Id;
            view.Width = image->Width;
            view.Height = image->Height;
            view.Fourcc = image->Fourcc;
            view.NativeBuffer = image->Native;
            if (!backend->BlitDefaultFramebufferToSharedImage(view, DamageOfRecord(op.DamageCount, op.Damage)))
                return false;
            fill(*image);
            return true;
        }
        case MG_Pipe::kMGPSharedImageQueryBufferAge: {
            MG_Backend::BackendObject* backend = ServerLoopInstance().Backend();
            if (backend == nullptr) return false;
            reply.BufferAge = backend->QueryCurrentBufferAge((op.Format & MG_Pipe::kMGPBufferAgeDamageRegionFollows) != 0);
            return true;
        }
        case MG_Pipe::kMGPSharedImageSetDamageRegion: {
            MG_Backend::BackendObject* backend = ServerLoopInstance().Backend();
            if (backend == nullptr) return false;
            return backend->SetCurrentDamageRegion(DamageOfRecord(op.DamageCount, op.Damage));
        }
        case MG_Pipe::kMGPSharedImageFlush: {
            // The answer is posted after the fence is published: the client commits the buffer to
            // its compositor only after this returns, so the compositor's session finds the write.
            MG_Backend::BackendObject* backend = ServerLoopInstance().Backend();
            if (backend == nullptr) return false;
            return backend->PublishSharedImageAccesses();
        }
        case MG_Pipe::kMGPSharedImageNativeFence: {
            // A descriptor of the session's work, and an image writer's flush (MGPipeTypes.h). Like
            // an Allocate's, the descriptor goes out BEFORE the reply is posted.
            MG_Backend::BackendObject* backend = ServerLoopInstance().Backend();
            if (backend == nullptr) return false;
            int fence = -1;
            if (!backend->ExportNativeFence(&fence)) return false;
#if !defined(_WIN32)
            if (fence >= 0) {
                m_lastNativeFence.Reset(::dup(fence));
            } else if (m_lastNativeFence.fd >= 0) {
                fence = ::dup(m_lastNativeFence.fd);
            }
#endif
            reply.Format = 0;
            if (fence < 0) return true;
            Transport::ITransport* transport = session->ControlTransport();
            MG_Pipe::MGPSharedImageFdOffer offer{MG_Pipe::kMGPSharedImageFdMagic, 1, seq, 0, 0};
            const MobileGLResult shared = transport != nullptr
                                              ? transport->ShareFd(fence, MobileGLByteSpan{&offer, sizeof(offer)})
                                              : MOBILEGL_ERR_NOT_INITIALIZED;
#if !defined(_WIN32)
            ::close(fence);
#endif
            if (shared != MOBILEGL_OK) {
                MGLOG_E_ONCE("MG_Remote server: a native fence could not be sent on the aux socket (rc=%d)",
                             static_cast<int>(shared));
                return false;
            }
            reply.Format = MG_Pipe::kMGPNativeFenceFdFollows;
            return true;
        }
        case MG_Pipe::kMGPSharedImageAttach: {
            SI::ImageRef image = holder.Get(op.ImageId);
            if (image == nullptr) return false;
            if (!MG_Pipe::MGPipeApplyAttachSharedImage(op.Texture, image->Id)) {
                MGLOG_E("MG_Remote server: shared image %llu could not be attached to texture {slot=%u, gen=%u}: the "
                        "texture has no record",
                        static_cast<unsigned long long>(image->Id), op.Texture.Slot, op.Texture.Gen);
                return false;
            }
            fill(*image);
            return true;
        }
        default:
            MGLOG_E("MG_Remote server: shared_image operation %u is not one this server knows", op.Op);
            return false;
        }
    }

    Bool ServerVerbSink::OnBindContext(const MG_Pipe::MGPBindContext& bind) {
        // NO SESSION IS A DECLINE, NOT A FAULT, and it is the one case here that a peer's bytes
        // cannot cause: a bind is answered by the session's own table, so with no accepted
        // session there is nothing to attribute the record to and nothing to refuse. (The window
        // is Close() clearing the active session while the apply thread is still draining.)
        ServerSession* session = ServerSession::Active();
        if (session == nullptr) return false;
        // PH-1 (3): the refusal latches in an armed session child (the peer wrote the token);
        // unarmed it dies, like every other contract fact about a peer.
        if (!session->BindContext(bind.ClientContextToken)) {
            return SessionLatch(MGFatalFamily::ProtocolCorruption, "MGPipe: Fatal{ProtocolCorruption, \"BindContext.ClientContextToken\"} - the "
                    "record binds token %llu, which this session never created; a bind_context "
                    "names a context CreateContext put in the session's table, and 0 (the "
                    "release) is the only token that needs no entry",
                    static_cast<unsigned long long>(bind.ClientContextToken));
        }
        ++m_bindContexts;
#if MOBILEGL_BUILD_DISAGGREGATED
        // P14 S4 (docs/Disaggregated/design/11-state-ownership.md). THE NATIVE HALF OF THE SWITCH.
        //
        // `bind_context` is where the session's context attribution moves, and it is also the ONLY
        // point at which the apply thread's native context may move: the MakeCurrent control frame
        // that precedes it still carries the OLD context (the client emits the binding after the
        // binding has taken effect, which is the ordering S1 chose on purpose), so a native switch
        // driven from there would bind the context the client just left. Frontend GL state is the
        // backend's per-tuple business; this call makes the tuple the resolver now names current.
        //
        // It is a no-op for the shapes S4 did not change: with no resolver installed (monolith, a
        // unit case, the inproc one-process shape) the key is {0,0}, and the single-context world's
        // bind does not move the native context - MakeNativeContextCurrentForBoundToken returns
        // without a driver call when the context it resolves to is already the one bound here.
        if (MG_Config::Transport != MG_Config::TransportMode::Monolith) {
            (void)MG_Backend::DirectGLES::MakeNativeContextCurrentForBoundToken();
            // The backend half for one that keeps a single native surface binding (Magma).
            if (MG_Backend::BackendObject* backend = ServerLoopInstance().Backend()) {
                backend->OnClientContextBound(bind.ClientContextToken);
            }
        }
#endif
        return true;
    }

    Bool ServerVerbSink::OnObjectDeath(const MG_Pipe::MGPHandleOnly& death) {
        // §1's zero ruling: a null handle means "the object never crossed", and the client
        // emits NOTHING in that case (§5.2) - so a null handle arriving here is corruption,
        // not a no-op.
        // PH-1 (3): both refusals latch in an armed session child; unarmed they die as before.
        if (MG_Pipe::MGPipeHandleIsNull(death.Handle)) {
            return SessionLatch(MGFatalFamily::ProtocolCorruption, "MGPipe: Fatal{ProtocolCorruption, \"ObjectDeath.Handle\"} - a null "
                    "handle never crosses: the client emits nothing for an object its own "
                    "allocator cannot resolve (CONTRACT-P5C.md §5.2)");
        }
        if (death.Kind >= static_cast<Uint32>(MG_Pipe::MGPipeKind::KindCount)) {
            return SessionLatch(MGFatalFamily::ProtocolCorruption, "MGPipe: Fatal{ProtocolCorruption, \"ObjectDeath.Kind\"} - %u is not an "
                    "MGPipeKind",
                    static_cast<unsigned>(death.Kind));
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

    void PipeApplier::Attach(Transport::ILink* link, MG_Backend::BackendObject* backend) {
        if (!link || !link->Attached() || !m_segments)
            SessionFail(MGFatalFamily::ProtocolCorruption, "MGPipe: Fatal{ProtocolCorruption, PipeApplier::Attach missing link}");
        m_verbs.SetBackend(backend);
        m_verbs.SetMaxReplyBytes(link->Capabilities().MaxReplyBytes);
        m_decoder = Wire::PipeWireDecoder(link, m_segments, m_replies);
        m_decoder.SetVerbSink(&m_verbs);
        MG_Pipe::MGPipeServerBlockNoteIdentity(); m_attached = true;
    }

    void PipeApplier::Attach(Transport::RingControl* control, MG_Backend::BackendObject* backend) {
        if (control == nullptr || m_segments == nullptr) {
            SessionFail(MGFatalFamily::ProtocolCorruption, "MGPipe: Fatal{ProtocolCorruption, \"PipeApplier::Attach\"} - no control "
                    "page or no segment table; ServerSession::Accept builds both before the "
                    "apply thread starts");
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
            SessionFail(MGFatalFamily::ProtocolCorruption, "MGPipe: Fatal{ProtocolCorruption, \"PipeApplier::ApplyOne before Attach\"} "
                    "- a record reached the applier with no decoder; the apply thread calls "
                    "Attach once before its first pop");
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
        //
        // PH-1 (3): BUT NOT BEFORE THE RECORD HAS BEEN ADMITTED. MGPipeBarriered reads payload
        // fields (a DrawVbo's MGPDrawInfo::Flags) and the stamps index per-opcode tables, so a
        // record whose ring header is shorter than its own type - or names no opcode - would be
        // read past its end here, before DecodeAndApply's pre-gate could refuse it. In an armed
        // session child the pre-gate therefore runs FIRST: a refused record latches by name and
        // is declined with nothing stamped (ServerLoopTest's short-record case reads the stamp).
        // Unarmed it admits everything and the generated gate keeps its death, as before.
        if (!m_decoder.AdmitOrDecline(record)) return false;
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
