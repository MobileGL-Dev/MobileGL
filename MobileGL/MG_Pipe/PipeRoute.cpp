// MobileGL - MobileGL/MG_Pipe/PipeRoute.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The MONOLITH arm of R-17's routing: thirty-eight adapters that unpack a generated table
// row's parameters and call the MGPipeApply* entry point the call site used to call directly.
// Owner: package c1. See PipeRoute.h for why the arm exists and what R-17 actually cost.
//
// G2 IS THE WHOLE SPECIFICATION OF THIS FILE. Under monolith transport the thunks must reach
// EXACTLY the code they reach today - so every adapter below is a parameter shuffle and
// nothing else. There is no branch, no cache, no early return and no logging on any of them,
// because each of those is a way for `MOBILEGL_TRANSPORT=monolith` to stop being the control
// arm that the split arm is measured against. The one thing an adapter may do beyond calling
// through is POST A REPLY, and only the five rows whose call site consumes a value do that.
//
// WHO CALLS THE INSTALL, AND WHY IT IS NOT IN THIS FILE. `gMGPipeScreen` and `gMGPipeContext`
// are inline variables with CONSTANT initialisation, so they are zero - every entry null,
// which is the pre-migration state - before any dynamic initialiser runs. The installer is
// driven from an inline variable in PipeRoute.h rather than from a static initialiser here,
// and the comment beside `detail::gMonolithTablesInstalled` says why: a static initialiser in
// THIS file is only in the program if THIS object file is in the link, and over a static
// archive it was not. That makes "the tables are installed" an invariant rather than a step
// someone can forget, and it is why no MGP_* thunk needs a null check - which matters, because
// a null check on a table slot is precisely the shape CONTRACT-P5 §7 spends the 41 call sites
// killing.

#include "PipeRoute.h"

#if MOBILEGL_PIPE_PUSH

#include <MG_Util/Debug/Log.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace MobileGL::MG_Pipe {

    namespace {

        // ---------------------------------------------------------------------------------
        // The reply mailbox
        // ---------------------------------------------------------------------------------

        struct ReplyMailbox {
            Uint64 Slot = 0;      // the MGPReplySlot::Id this answer belongs to; 0 = empty
            Int32 Status = 0;     // 0 OK, 1 DECLINED, 2 ERROR (Transport::ReplyStatus)
            Uint64 Value = 0;     // the Bool, or the pointer, the row answered with
            Uint64 NextTicket = 0;
            Uint64 Taken = 0;
            Uint64 Declined = 0;
        };

        // THREAD-LOCAL, and one entry deep, because R-1's verb barrier makes the in-flight
        // depth exactly one. A second post before the first is taken is not a capacity problem:
        // it is a barrier that has stopped holding, and it Fatals rather than overwriting.
        thread_local ReplyMailbox g_reply;

        MGPipeRouteArm g_arm = MGPipeRouteArm::kNone;

        // The monolith adapters, kept as tables of their own so the split arm can reach them
        // on the server role's thread. See PipeRoute.h.
        MGPipeScreen g_monolithScreen{};
        MGPipeContext g_monolithContext{};
        MGPipeRouteEscapes g_monolithEscapes{};

    } // namespace

    const MGPipeScreen& MGPipeMonolithScreen() { return g_monolithScreen; }
    const MGPipeContext& MGPipeMonolithContext() { return g_monolithContext; }
    const MGPipeRouteEscapes& MGPipeMonolithEscapes() { return g_monolithEscapes; }

    MGPReplySlot MGPipeMintReplySlot() {
        // 1-based, for ReplySlot.h's reason: "Seq is 1-based; 0 means 'no record'". A zero id
        // must stay unmintable so an un-posted mailbox is distinguishable from a posted one.
        return MGPReplySlot{++g_reply.NextTicket};
    }

    void MGPipePostReply(const MGPReplySlot& slot, Int32 status, Uint64 value) {
        if (slot.Id == 0) {
            MGLOG_F("MGPipe: Fatal{ReplyToNoSlot} - a table row posted an answer against reply "
                    "slot 0, which ReplySlot.h reserves for \"no record\"");
            std::abort();
        }
        if (g_reply.Slot != 0 && g_reply.Slot != slot.Id) {
            MGLOG_F("MGPipe: Fatal{ReplyOverrun} - slot %llu posted while slot %llu was still "
                    "unread. The mailbox is one deep because R-1's verb barrier makes the "
                    "in-flight depth one; two outstanding answers means the barrier is not "
                    "holding",
                    static_cast<unsigned long long>(slot.Id),
                    static_cast<unsigned long long>(g_reply.Slot));
            std::abort();
        }
        g_reply.Slot = slot.Id;
        g_reply.Status = status;
        g_reply.Value = value;
    }

    namespace {
        // The one reader. Fatals rather than defaulting, in both directions - see PipeRoute.h.
        Uint64 TakeReply(const MGPReplySlot& slot, const char* row, Int32* statusOut) {
            if (g_reply.Slot == 0) {
                MGLOG_F("MGPipe: Fatal{ReplyMissing, \"%s\"} - the row was routed and nothing "
                        "posted an answer. R-5 forbids re-deriving acceptance on the client and "
                        "forbids assuming it, so there is no default to fall back to",
                        row);
                std::abort();
            }
            if (g_reply.Slot != slot.Id) {
                MGLOG_F("MGPipe: Fatal{ReplyMismatched, \"%s\"} - waiting on slot %llu, the "
                        "mailbox holds slot %llu",
                        row, static_cast<unsigned long long>(slot.Id),
                        static_cast<unsigned long long>(g_reply.Slot));
                std::abort();
            }
            const Uint64 value = g_reply.Value;
            if (statusOut != nullptr) *statusOut = g_reply.Status;
            if (g_reply.Status == 1) ++g_reply.Declined;
            ++g_reply.Taken;
            g_reply.Slot = 0;
            g_reply.Value = 0;
            g_reply.Status = 0;
            return value;
        }
    } // namespace

    Bool MGPipeTakeReplyBool(const MGPReplySlot& slot, const char* row) {
        Int32 status = 0;
        const Uint64 value = TakeReply(slot, row, &status);
        // DECLINED IS A REAL ANSWER AND IT IS `false`, not a failure (ReplySlot.h). ERROR is
        // not an acceptance answer at all and may not be folded into either.
        if (status == 2) {
            MGLOG_F("MGPipe: Fatal{ReplyError, \"%s\"} - the row answered ERROR, which is not an "
                    "acceptance answer; folding it into accepted or refused would make a "
                    "transport fault look like a resource decision",
                    row);
            std::abort();
        }
        if (status == 1) return false;
        return value != 0;
    }

    void* MGPipeTakeReplyPointer(const MGPReplySlot& slot, const char* row) {
        Int32 status = 0;
        const Uint64 value = TakeReply(slot, row, &status);
        if (status == 2) {
            MGLOG_F("MGPipe: Fatal{ReplyError, \"%s\"}", row);
            std::abort();
        }
        // DECLINED is the null pointer, and it is R-6's answer for map_persistent under split.
        if (status == 1) return nullptr;
        return reinterpret_cast<void*>(static_cast<std::uintptr_t>(value));
    }

    Uint64 MGPipeRepliesTaken() { return g_reply.Taken; }
    Uint64 MGPipeRepliesDeclined() { return g_reply.Declined; }

    MGPipeRouteArm MGPipeInstalledArm() { return g_arm; }
    void MGPipeNoteInstalledArm(MGPipeRouteArm arm) { g_arm = arm; }
    Bool MGPipeTablesAreInstalled() { return g_arm != MGPipeRouteArm::kNone; }

    // ---------------------------------------------------------------------------------
    // The thirty-eight monolith adapters
    // ---------------------------------------------------------------------------------
    //
    // Three shapes, so the eye can check them against PipeTables.inc in one pass rather than
    // reading thirty-eight bodies. A row that does not fit one of the three is written out by
    // hand BELOW the macros, never by widening a macro - a macro that grew a special case is
    // how one of these silently stops being a parameter shuffle.

    namespace {

#define MGP_MONO_PLAIN(Name, Payload)                                                              \
    void Mono_##Name(const Payload* payload) { MGPipeApply##Name(*payload); }

#define MGP_MONO_BLOB(Name, Payload)                                                               \
    void Mono_##Name(const Payload* payload, const void* blobBytes, Uint64) {                      \
        MGPipeApply##Name(*payload, blobBytes);                                                    \
    }

#define MGP_MONO_TAIL(Name, Payload, TailType)                                                     \
    void Mono_##Name(const Payload* payload, const void* varTail, Uint32) {                        \
        MGPipeApply##Name(*payload, static_cast<const TailType*>(varTail));                        \
    }

        // -- screen -------------------------------------------------------------------
        MGP_MONO_PLAIN(ResourceDestroy, MGPHandleOnly)
        MGP_MONO_PLAIN(UnmapPersistent, MGPHandleOnly)

        // -- context, plain -----------------------------------------------------------
        MGP_MONO_PLAIN(BindRenderState, MGPBindRenderState)
        MGP_MONO_PLAIN(DeleteRenderState, MGPHandleOnly)
        MGP_MONO_PLAIN(BindVertexElements, MGPHandleOnly)
        MGP_MONO_PLAIN(DeleteVertexElements, MGPHandleOnly)
        MGP_MONO_PLAIN(DeleteSamplerState, MGPHandleOnly)
        MGP_MONO_PLAIN(CreateSamplerView, MGPSamplerView)
        MGP_MONO_PLAIN(DeleteSamplerView, MGPHandleOnly)
        MGP_MONO_PLAIN(BindShaderState, MGPHandleOnly)
        MGP_MONO_PLAIN(DeleteShaderState, MGPHandleOnly)
        MGP_MONO_PLAIN(SetDrawProgram, MGPHandleOnly)
        MGP_MONO_PLAIN(SetDispatchProgram, MGPHandleOnly)
        MGP_MONO_PLAIN(SetFramebufferState, MGPFramebufferState)
        MGP_MONO_PLAIN(SetIndexBuffer, MGPIndexBuffer)
        MGP_MONO_PLAIN(SetPixelPackState, MGPPixelPackState)
        MGP_MONO_PLAIN(SetPatchState, MGPPatchState)
        // P5c (rv): dormant under monolith - PipeFill.cpp's producer is transport-gated, so
        // nothing routes here without a live wire. It exists because the row has an
        // MGPipeApply* entry point (unlike the two control records) and because the wire
        // emitter's server-role arm forwards to this table (WireTables.cpp).
        MGP_MONO_PLAIN(SetContextValues, MGPContextValues)

        // -- context, blob companion --------------------------------------------------
        MGP_MONO_BLOB(CreateRenderState, MGPRenderStateDesc)
        MGP_MONO_BLOB(CreateVertexElements, MGPVertexElements)
        MGP_MONO_BLOB(SetDynamicState, MGPDynamicState)
        MGP_MONO_BLOB(SetGlobalConstants, MGPGlobalConstants)
        MGP_MONO_BLOB(BufferSubDataResident, MGPSubData)

        // -- context, variable tail ---------------------------------------------------
        MGP_MONO_TAIL(SetVertexBuffers, MGPVertexBuffers, MGPVertexBuffer)
        MGP_MONO_TAIL(SetSamplerViews, MGPSamplerViews, MGPBoundView)
        MGP_MONO_TAIL(BindSamplerStates, MGPSamplerStates, MGPipeHandle)
        MGP_MONO_TAIL(SetShaderImages, MGPShaderImages, MGPImageView)
        // P5e (sb, CONTRACT-P5E.md §5.6). Catalogued since P4a with no producer and no
        // consumer; the adapter lands with the applier's body. The generic tail shape fits
        // because the SECOND tail is absent on every Espryt configuration (HostSpanCount is 0
        // while kCapNeedsHostUboBytes is 0, which is the whole of P5).
        MGP_MONO_TAIL(SetShaderBuffers, MGPShaderBuffers, MGPBufferRange)
        MGP_MONO_TAIL(SetVertexAttribDefaults, MGPVertexAttribDefaults, MGPAttribValue)

#undef MGP_MONO_PLAIN
#undef MGP_MONO_BLOB
#undef MGP_MONO_TAIL

        // -- the rows that fit none of the three shapes --------------------------------

        // create_sampler_state: the blob is a TYPED frontend struct, not bytes the applier
        // reads through a void*. The cast is the whole adapter, and it is exact: the client
        // stages sizeof(SamplerParameters) bytes of the same object (CONTRACT-P5 table 1
        // row 17, including its padding trap - the bytes staged must be the bytes a later
        // memcmp compares).
        void Mono_CreateSamplerState(const MGPSamplerDesc* payload, const void* blobBytes, Uint64) {
            MGPipeApplyCreateSamplerState(*payload,
                                          static_cast<const SamplerParameters*>(blobBytes));
        }

        // set_residual_value_state: CONTRACT-P5 table 1 row 6, "the hardest row in the table".
        // The applier takes `const ResidualValueBlock&` - a frontend type - and
        // MGPResidualValueState is never instantiated on the live path. So the BLOCK IS THE
        // BLOB, in both arms, and the adapter copies it back out. Requiring exact equality
        // rather than ">=" is the decoder's rule too (PipeWireCodec.cpp): a size that only
        // ever ratchets down makes a short read silently lose CapabilityBits.
        void Mono_SetResidualValueState(const MGPResidualValueState*, const void* blobBytes,
                                        Uint64 blobByteCount) {
            if (blobByteCount != sizeof(ResidualValueBlock) || blobBytes == nullptr) {
                MGLOG_F("MGPipe: Fatal{ResidualBlockSize} - set_residual_value_state carries %llu "
                        "bytes, the block is %llu",
                        static_cast<unsigned long long>(blobByteCount),
                        static_cast<unsigned long long>(sizeof(ResidualValueBlock)));
                std::abort();
            }
            ResidualValueBlock block{};
            std::memcpy(&block, blobBytes, sizeof(block));
            MGPipeApplySetResidualValueState(block);
        }

        // resource_readback carries kReplySlot but its answer is COMPLETION, not a value: the
        // bytes go server -> client through SEG_EVENT's OnBufferWriteback (CONTRACT-P5 table 1
        // row 22), and the split decoder posts exactly kStatusOk with a zero-length payload.
        // The monolith arm says the same thing, so the two arms hand their caller the same
        // answer rather than one of them handing it nothing.
        void Mono_ResourceReadback(const MGPReadback* payload, MGPReplySlot* reply) {
            MGPipeApplyResourceReadback(*payload);
            MGPipePostReply(*reply, 0, 0);
        }

        // The three acceptance rows that DO fit a generated signature. Each posts the
        // applier's own answer; none of them invents one.
        void Mono_ResourceCreate(const MGPResourceDesc* payload, MGPReplySlot* reply) {
            const Bool accepted = MGPipeApplyResourceCreate(*payload);
            MGPipePostReply(*reply, accepted ? 0 : 1, accepted ? 1u : 0u);
        }

        void Mono_SetTextureParams(const MGPTextureParams* payload, MGPReplySlot* reply) {
            const Bool accepted = MGPipeApplySetTextureParams(*payload);
            MGPipePostReply(*reply, accepted ? 0 : 1, accepted ? 1u : 0u);
        }

        void Mono_ResourceSubData(const MGPSubData* payload, const void* blobBytes, Uint64,
                                  const void* varTail, Uint32, MGPReplySlot* reply) {
            const Bool accepted = MGPipeApplyResourceSubData(
                *payload, blobBytes, static_cast<const MGPSubRegion*>(varTail));
            MGPipePostReply(*reply, accepted ? 0 : 1, accepted ? 1u : 0u);
        }

        // -- the four escapes ----------------------------------------------------------

        Bool Mono_Escape_ResourceRespecify(const MGPResourceDesc* desc, const void* initialBytes,
                                           const MGPRespecifiedLevel* level) {
            return MGPipeApplyResourceRespecify(*desc, initialBytes, level);
        }

        void Mono_Escape_ResourceFlushRange(const MGPFlushRange* record, const void* bytes) {
            MGPipeApplyResourceFlushRange(*record, bytes);
        }

        void* Mono_Escape_MapPersistent(const MGPHandleOnly* handle, Uint64 size,
                                        const void* seedBytes) {
            return MGPipeApplyMapPersistent(*handle, size, seedBytes);
        }

        void Mono_Escape_CreateShaderState(const MGPProgramDesc* desc,
                                           const MG_State::GLState::LinkArtifacts* link,
                                           const MG_State::GLState::SpirvArtifacts* spirv) {
            MGPipeApplyCreateShaderState(*desc, link, spirv);
        }

    } // namespace

    void MGPipeInstallMonolithTables() {
        gMGPipeScreen.ResourceCreate = &Mono_ResourceCreate;
        gMGPipeScreen.ResourceDestroy = &Mono_ResourceDestroy;
        gMGPipeScreen.UnmapPersistent = &Mono_UnmapPersistent;

        gMGPipeContext.CreateRenderState = &Mono_CreateRenderState;
        gMGPipeContext.BindRenderState = &Mono_BindRenderState;
        gMGPipeContext.DeleteRenderState = &Mono_DeleteRenderState;
        gMGPipeContext.CreateVertexElements = &Mono_CreateVertexElements;
        gMGPipeContext.BindVertexElements = &Mono_BindVertexElements;
        gMGPipeContext.DeleteVertexElements = &Mono_DeleteVertexElements;
        gMGPipeContext.CreateSamplerState = &Mono_CreateSamplerState;
        gMGPipeContext.DeleteSamplerState = &Mono_DeleteSamplerState;
        gMGPipeContext.CreateSamplerView = &Mono_CreateSamplerView;
        gMGPipeContext.DeleteSamplerView = &Mono_DeleteSamplerView;
        gMGPipeContext.BindShaderState = &Mono_BindShaderState;
        gMGPipeContext.DeleteShaderState = &Mono_DeleteShaderState;
        gMGPipeContext.SetDrawProgram = &Mono_SetDrawProgram;
        gMGPipeContext.SetDispatchProgram = &Mono_SetDispatchProgram;
        gMGPipeContext.SetDynamicState = &Mono_SetDynamicState;
        gMGPipeContext.SetFramebufferState = &Mono_SetFramebufferState;
        gMGPipeContext.SetVertexBuffers = &Mono_SetVertexBuffers;
        gMGPipeContext.SetIndexBuffer = &Mono_SetIndexBuffer;
        gMGPipeContext.SetSamplerViews = &Mono_SetSamplerViews;
        gMGPipeContext.BindSamplerStates = &Mono_BindSamplerStates;
        gMGPipeContext.SetShaderImages = &Mono_SetShaderImages;
        gMGPipeContext.SetShaderBuffers = &Mono_SetShaderBuffers;
        gMGPipeContext.SetGlobalConstants = &Mono_SetGlobalConstants;
        gMGPipeContext.SetVertexAttribDefaults = &Mono_SetVertexAttribDefaults;
        gMGPipeContext.SetPixelPackState = &Mono_SetPixelPackState;
        gMGPipeContext.SetPatchState = &Mono_SetPatchState;
        gMGPipeContext.SetContextValues = &Mono_SetContextValues;
        gMGPipeContext.SetResidualValueState = &Mono_SetResidualValueState;
        gMGPipeContext.SetTextureParams = &Mono_SetTextureParams;
        gMGPipeContext.ResourceSubData = &Mono_ResourceSubData;
        gMGPipeContext.BufferSubDataResident = &Mono_BufferSubDataResident;
        gMGPipeContext.ResourceReadback = &Mono_ResourceReadback;

        gMGPipeRouteEscapes.ResourceRespecify = &Mono_Escape_ResourceRespecify;
        gMGPipeRouteEscapes.ResourceFlushRange = &Mono_Escape_ResourceFlushRange;
        gMGPipeRouteEscapes.MapPersistent = &Mono_Escape_MapPersistent;
        gMGPipeRouteEscapes.CreateShaderState = &Mono_Escape_CreateShaderState;

        // KEPT, not merely installed. The client arm overwrites the three tables above; these
        // three copies are what it forwards to when it finds itself on the server's own thread.
        g_monolithScreen = gMGPipeScreen;
        g_monolithContext = gMGPipeContext;
        g_monolithEscapes = gMGPipeRouteEscapes;

        MGPipeNoteInstalledArm(MGPipeRouteArm::kMonolith);
    }

} // namespace MobileGL::MG_Pipe

#endif // MOBILEGL_PIPE_PUSH
