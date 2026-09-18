// MobileGL - MobileGL/MG_Pipe/PipeRoute.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// THE CLIENT -> WIRE ROUTING OF THE 38 MGPipeApply* ENTRY POINTS (P5, integrator ruling R-17;
// 38 since P5c rv added set_context_values, CONTRACT-P5C.md section 5.3).
// Owner: package c1.
//
// WHAT WAS MISSING. `gMGPipeWireRecordApply` is the DECODE hook and it has existed since w1:
// a record arrives, `PipeWireCodec` picks it apart and calls `MGPipeApply<Name>`. There was no
// encode twin. Every `MG_Impl/Pipe` emitter called `MGPipeApply<Name>` DIRECTLY, so under
// `MOBILEGL_TRANSPORT=inproc` every resource, CSO, texture and program record still executed
// synchronously on the GL thread against a context v1 is moving to the apply thread - where
// all sixteen `IsBackendContextCurrentOnThisThread()` and sixteen `CanTouchGLNow()` guards
// answer false. `ClearThenReadPixels` might have survived that; `TriangleScenario` needs a VBO
// and a program and could not.
//
// The two generated tables (`gMGPipeScreen`, `gMGPipeContext`, `generated/PipeTables.inc`) are
// the boundary this repository already had, and they were never installed - every entry null,
// which `MGPipe.h` correctly calls "precisely the pre-migration state". R-17 installs them.
// This header is the frontend-facing half: the names `MG_Impl/Pipe` calls, and the mechanism
// that gets a kReplySlot row's answer back to its call site.
//
// ---------------------------------------------------------------------------------------
// THE SIZE OF R-17, MEASURED RATHER THAN ESTIMATED
// ---------------------------------------------------------------------------------------
//
// R-17 costed this as "37 thin client emitters over EmitAndWait". The emitters are thin. The
// ROUTING was not, and the reason is that the generated table signature is the P2-era monolith
// interface: `void (*Name)(const Payload* payload, ...)`, async-with-handle, no return value,
// and NO COMPANION POINTER. Of the 37 entry points, as the tables stood:
//
//     24  the row could carry every argument the call site passes today
//     13  it could not
//
// The thirteen split three ways, and only one of the three is a generator question:
//
//  (a) NINE carry a blob companion - `chunkBytes`, `blobBytes`, `parameters`, `bytes`,
//      `initialBytes` - which is CONTRACT-P5 table 1's "companion pointer today" column. A
//      kVarTail row already gains `const void* varTail, Uint32 varTailCount`; a kHasBlob row
//      gained nothing. THE GENERATOR CHANGE IS THAT ONE ASYMMETRY, and nothing else: five
//      lines in `Call.Signature` giving a kHasBlob row `const void* blobBytes, Uint64
//      blobByteCount`. Ten rows gained the pair (the nine plus `GetCaps`, which has no applier
//      entry point). `--check` up to date, `--self-test` 9/9 trips.
//
//  (b) FOUR return a value the row cannot: `ResourceCreate`, `ResourceRespecify`,
//      `ResourceSubData` and `SetTextureParams` return Bool and `MapPersistent` returns void*.
//      These are exactly CONTRACT-P5 §2's reply-slot rows, and R-5 forbids re-deriving any of
//      them on the client. The answer therefore has to come back THROUGH the reply slot, which
//      is what `MGPipeTakeReply*` below is, and it is the part of R-17 that no amount of
//      "thin emitter" covers: `MGPReplySlot` is `{Uint64 Id;}` - an IDENTIFIER, not a value -
//      so a reader had to exist for the identifier to be worth minting.
//
//  (c) FOUR CANNOT GO THROUGH A GENERATED ROW AT ALL, and each one is a contract ruling
//      rather than an oversight. They are the ESCAPES below. Saying "four" out loud is the
//      honest answer to "tell me if it is bigger than it looks": it is, by four rows and one
//      reply-reading mechanism.
//
// ---------------------------------------------------------------------------------------
// THE REPLY MAILBOX, AND WHY ONE ENTRY IS THE RIGHT DEPTH
// ---------------------------------------------------------------------------------------
//
// `MGPReplySlot` carries an id; the answer lands beside it. The mailbox is ONE ENTRY DEEP and
// thread-local, and that is not a simplification - it is R-1's verb barrier stated as a data
// structure. While the barrier holds, the in-flight depth is exactly one (`ReplySlot.h`:
// "under R-1's verb barrier the in-flight depth is one"), so a second posting before the first
// is taken is not a capacity problem, it is a barrier that has stopped working. It Fatals.
//
// THE MAILBOX HAS NO DEFAULT ANSWER AND THIS IS THE WHOLE POINT. `MGPipeTakeReplyBool` on a
// slot nothing posted to is `Fatal{ReplyMissing, "<row>"}`, not `false` and emphatically not
// `true`. "Always accept" is ID-39's 66 lost DirectVulkan uploads with a wire in between, and
// "always refuse" is an emitter that re-sends for ever. A row that forgets to answer is a bug
// that must be impossible to ship, so it is impossible to READ.
//
// WHEN THE BARRIER RETIRES (R-1 opens family by family) this becomes a real slot pool keyed on
// seq, which is what `Transport::ReplySlot` already is on the wire side. The mailbox is the
// frontend-side handle onto it and nothing else; it is deliberately NOT a second id space,
// because the split arm stamps the record's own seq into `MGPReplySlot::Id` (R-3).

#pragma once
#include <Includes.h>

#if MOBILEGL_PIPE_PUSH

#include "MGPipe.h"
#include "PipeApply.h"

namespace MobileGL::MG_Pipe {

    // ---------------------------------------------------------------------------------
    // The reply mailbox
    // ---------------------------------------------------------------------------------

    // Mints the id for one call's answer. Monolith's ids and the wire's seqs are separate
    // spaces on purpose: under split the emitter OVERWRITES this with the record's own seq
    // (R-3: "the reply slot id IS the record sequence number"), so the id a caller finally
    // reads is the wire's, and under monolith it is a local ticket that only has to be unique
    // against the one outstanding call the barrier permits.
    MGPReplySlot MGPipeMintReplySlot();

    // Posts one answer. Called by whichever table is installed - the monolith adapter with
    // the applier's return value, the client emitter with the bytes the server put in
    // SEG_REPLY. `status` is Transport::ReplyStatus' value space (0 OK, 1 DECLINED, 2 ERROR),
    // restated as a plain Int32 so MG_Pipe does not have to see MG_Remote at all.
    void MGPipePostReply(const MGPReplySlot& slot, Int32 status, Uint64 value);

    // Takes the answer for `slot`. Fatals - naming `row` - if nothing was posted, if what was
    // posted belongs to a different slot, or if the answer has already been taken.
    Bool MGPipeTakeReplyBool(const MGPReplySlot& slot, const char* row);
    void* MGPipeTakeReplyPointer(const MGPReplySlot& slot, const char* row);

    // How many answers this thread has taken, and how many of those were DECLINED. Counted
    // rather than inferred, for R-8's reason one level out: "the client accepted everything"
    // and "the client never asked" are otherwise the same observation from outside.
    Uint64 MGPipeRepliesTaken();
    Uint64 MGPipeRepliesDeclined();

    // ---------------------------------------------------------------------------------
    // The escapes: the four rows no generated signature can express
    // ---------------------------------------------------------------------------------
    //
    // A hand-written third table, installed and uninstalled by the same two functions as the
    // generated pair, so there is ONE mechanism and not two. Each row's signature is the
    // applier's own, because that is the shape the monolith arm has to reproduce byte for
    // byte (G2), and each row is here for a ruling that is written down:
    //
    //   ResourceRespecify  `initialBytes` has NO kHasBlob and R-13.3 forbids giving it one
    //                      ("initialBytes is always nullptr under split; initial content
    //                      arrives as ResourceSubData records immediately after this one").
    //                      So the row is correct to omit it AND monolith must still pass it.
    //   ResourceFlushRange `bytes` likewise, by R-13.2, and for a sharper reason: a blobref
    //                      here would be "a second, forgeable way to say the same thing".
    //   MapPersistent      `size` and `seedBytes` have no carrier at all - `MGPHandleOnly` is
    //                      {Handle, Kind} - and the call returns void*. R-6 makes the split
    //                      answer a constant DECLINE, so the carrier is not needed; the
    //                      monolith arm needs both arguments.
    //   CreateShaderState  SEVEN blobrefs (`Spirv[6]` + `Reflection`) and TWO typed frontend
    //                      pointers. One `blobBytes` pair cannot express seven runs, and
    //                      serialising under monolith to un-serialise in the adapter would put
    //                      `EncodeProgramArtifacts` on the monolith path, which `PipeApply.h`
    //                      explicitly promises it is not ("zero serialisation cost on the
    //                      monolith path").
    //
    // OVERTURN CONDITIONS, one per row: give `MGPResourceDesc` and `MGPFlushRange` a blobref
    // (overturns R-13.2/R-13.3, and c0 owns `MGPipeTypes.h`); give `MGPHandleOnly` a size
    // (same owner) and P6 a real remote map; measure that a per-stage SPIR-V run beats one
    // archive, at which point `CreateShaderState` needs a multi-blob row rather than this one.
    struct MGPipeRouteEscapes {
        Bool (*ResourceRespecify)(const MGPResourceDesc* desc, const void* initialBytes,
                                  const MGPRespecifiedLevel* level);
        void (*ResourceFlushRange)(const MGPFlushRange* record, const void* bytes);
        void* (*MapPersistent)(const MGPHandleOnly* handle, Uint64 size, const void* seedBytes);
        void (*CreateShaderState)(const MGPProgramDesc* desc,
                                  const MG_State::GLState::LinkArtifacts* link,
                                  const MG_State::GLState::SpirvArtifacts* spirv);
    };
    inline MGPipeRouteEscapes gMGPipeRouteEscapes{};

    // ---------------------------------------------------------------------------------
    // The two install functions
    // ---------------------------------------------------------------------------------

    // Installs the monolith adapters into all three tables. Idempotent.
    void MGPipeInstallMonolithTables();

    // The monolith adapters, kept beside the installed tables rather than only written into
    // them. THE SPLIT ARM NEEDS THEM AT RUNTIME, and the reason is table 3's role split made
    // concrete: `gMGPipeScreen` / `gMGPipeContext` are PROCESS globals, and under `inproc` the
    // server role lives in the same process on the apply thread. When that thread runs the
    // server's own backend - the EGL bring-up, InitCapabilities, or the applier itself - it
    // reaches the very same `MG_Impl/Pipe` emitters the client does, and a wire emitter there
    // publishes a record and then waits for the apply thread to apply it. That thread IS the
    // apply thread, so it waits for itself: `Fatal{BarrierTimeout, "ResourceRespecify"}`,
    // logged by `mgl-srv-apply`, thirty seconds after bring-up starts.
    //
    // So the client emitters ask "am I the server role right now?" and, if so, run the
    // monolith adapter - which is exactly what `PipeWireCodec` already does on the decode side
    // by calling `MGPipeApply*` directly. The predicate is v1's `ServerLoop::OnApplyThread()`;
    // these three accessors are what makes the other half reachable.
    const MGPipeScreen& MGPipeMonolithScreen();
    const MGPipeContext& MGPipeMonolithContext();
    const MGPipeRouteEscapes& MGPipeMonolithEscapes();

    namespace detail {
        // THE INSTALL THAT CANNOT BE LINKED AWAY, and the first version of this WAS.
        //
        // It began as a static initialiser inside PipeRoute.cpp, on the reasoning that
        // `gMGPipeScreen` and `gMGPipeContext` are inline variables with CONSTANT (zero)
        // initialisation - sequenced before every dynamic initialiser - so an installer in
        // dynamic init necessarily beats any GL entry point. That reasoning is correct and the
        // mechanism still failed, because it assumed PipeRoute.o would be in the link at all:
        // every MG_Test target is its OWN binary over a static archive, `CsoCache.h` reaches
        // the table through an inline thunk and names no symbol from PipeRoute.o, so the
        // linker dropped the object, the initialiser never ran, and five CsoCacheTest cases
        // took a null function pointer. The shared library linked it and was fine, which is
        // exactly the shape that ships.
        //
        // AN INLINE VARIABLE FIXES BOTH HALVES AT ONCE. Its initialiser is emitted as a COMDAT
        // in every translation unit that includes this header, so any binary that can call a
        // thunk has one copy of it; and naming `MGPipeInstallMonolithTables` from a header the
        // call sites already include is an undefined reference that forces the archive member
        // into the link. There is still exactly one install, and it still happens before main.
        inline const Bool gMonolithTablesInstalled = (MGPipeInstallMonolithTables(), true);
    } // namespace detail

    // True once the tables hold something. Exposed for the gates only: a probe that armed
    // against an un-installed table would be arming against the pre-migration state.
    Bool MGPipeTablesAreInstalled();

    // Which arm is installed, so a case can assert the arm it thinks it is testing rather than
    // trusting `MOBILEGL_TRANSPORT`. `MG_Config::Transport` says what was ASKED for; this says
    // what the table actually does.
    enum class MGPipeRouteArm : Uint8 { kNone, kMonolith, kClientWire };
    MGPipeRouteArm MGPipeInstalledArm();
    void MGPipeNoteInstalledArm(MGPipeRouteArm arm);

    // ---------------------------------------------------------------------------------
    // The call-site names: MGPipeRoute<Name> for each of the thirty-eight
    // ---------------------------------------------------------------------------------
    //
    // EVERY ONE TAKES THE APPLIER'S OWN SIGNATURE, so converting a call site is a rename and
    // nothing else. That is not tidiness, it is the G1 lesson from round 1 stated as a rule:
    // rewriting `if (const auto f = TABLE.GL.Slot)` into a Bool-valued macro is semantically
    // identical and generates DIFFERENT code (-144 bytes, two symbols resized). A wrapper that
    // took `&expr` would force every site holding a temporary - `HandleOnly(handle)`,
    // `BufferHandleOnly(handle)` - to grow a named local, which is a second expression shape
    // change at forty sites. A reference parameter keeps the call site's text identical but
    // for the name.
    //
    // THREE OF THE THIRTY-SEVEN TAKE ONE ARGUMENT MORE THAN THEIR APPLIER, and the extra is
    // always the same thing: A BYTE COUNT THE RECORD DOES NOT DECLARE. CONTRACT-P5 table 1
    // rule A requires every kHasBlob row to declare its length under split; five rows already
    // do and the wrapper reads it back off the record, three do not and their call site is the
    // only place the number exists:
    //     SetGlobalConstants     declares 0 (ProgramEmit.h); the length is GetUBOSize()
    //     ResourceSubData        buffer half declares real, TEXTURE half declares 0 on the
    //                            grounds that the count is "the server's to compute" - which
    //                            table 1 row 7 says "cannot be a bounds check". The level
    //                            shadow's byte size is in scope at that call site.
    //     BufferSubDataResident  the application's staging store, sized by the caller
    // A defaulted argument was rejected: a default would let a site that has the number forget
    // to pass it and still compile, and the failure would be a silently short upload.

    // ---- screen ------------------------------------------------------------------------
    inline Bool MGPipeRouteResourceCreate(const MGPResourceDesc& desc) {
        MGPReplySlot reply = MGPipeMintReplySlot();
        MGP_ResourceCreate(&desc, &reply);
        return MGPipeTakeReplyBool(reply, "resource_create");
    }
    inline Bool MGPipeRouteResourceRespecify(const MGPResourceDesc& desc, const void* initialBytes,
                                             const MGPRespecifiedLevel* level = nullptr) {
        return gMGPipeRouteEscapes.ResourceRespecify(&desc, initialBytes, level);
    }
    inline void MGPipeRouteResourceDestroy(const MGPHandleOnly& handle) {
        MGP_ResourceDestroy(&handle);
    }
    inline void* MGPipeRouteMapPersistent(const MGPHandleOnly& handle, Uint64 size,
                                          const void* seedBytes) {
        return gMGPipeRouteEscapes.MapPersistent(&handle, size, seedBytes);
    }
    inline void MGPipeRouteUnmapPersistent(const MGPHandleOnly& handle) {
        MGP_UnmapPersistent(&handle);
    }

    // ---- resources ---------------------------------------------------------------------
    inline Bool MGPipeRouteResourceSubData(const MGPSubData& record, const void* bytes,
                                           Uint64 byteCount,
                                           const MGPSubRegion* regions = nullptr) {
        MGPReplySlot reply = MGPipeMintReplySlot();
        MGP_ResourceSubData(&record, bytes, byteCount, regions, record.RegionCount, &reply);
        return MGPipeTakeReplyBool(reply, "resource_subdata");
    }
    inline void MGPipeRouteBufferSubDataResident(const MGPSubData& record, const void* bytes,
                                                 Uint64 byteCount) {
        MGP_BufferSubDataResident(&record, bytes, byteCount);
    }
    inline void MGPipeRouteResourceFlushRange(const MGPFlushRange& record, const void* bytes) {
        gMGPipeRouteEscapes.ResourceFlushRange(&record, bytes);
    }
    inline void MGPipeRouteResourceReadback(const MGPReadback& record) {
        MGPReplySlot reply = MGPipeMintReplySlot();
        MGP_ResourceReadback(&record, &reply);
        // The answer is COMPLETION, not a value: the bytes come back through SEG_EVENT's
        // OnBufferWriteback (table 1 row 22). It is still TAKEN, because an untaken mailbox
        // entry is what the next row's ReplyOverrun Fatal is looking for.
        (void)MGPipeTakeReplyBool(reply, "resource_readback");
    }

    // ---- render state ------------------------------------------------------------------
    inline void MGPipeRouteCreateRenderState(const MGPRenderStateDesc& desc,
                                             const void* chunkBytes) {
        MGP_CreateRenderState(&desc, chunkBytes, desc.Blob.Size);
    }
    inline void MGPipeRouteBindRenderState(const MGPBindRenderState& bind) {
        MGP_BindRenderState(&bind);
    }
    inline void MGPipeRouteDeleteRenderState(const MGPHandleOnly& handle) {
        MGP_DeleteRenderState(&handle);
    }
    inline void MGPipeRouteSetDynamicState(const MGPDynamicState& dyn, const void* chunkBytes) {
        MGP_SetDynamicState(&dyn, chunkBytes, dyn.Blob.Size);
    }
    inline void MGPipeRouteSetPixelPackState(const MGPPixelPackState& pack) {
        MGP_SetPixelPackState(&pack);
    }
    inline void MGPipeRouteSetPatchState(const MGPPatchState& patch) { MGP_SetPatchState(&patch); }
    // P5c (rv, CONTRACT-P5C.md §5.3): emitted ONLY with an active transport (PipeFill.cpp
    // gates on it); under monolith the row is never produced and the fields keep coming
    // through the residual fill (G1). The row IS an ordinary routed set_* otherwise - the
    // monolith adapter exists so the wire emitter's server-role arm has something defined to
    // forward to, and the catalogue's null-row accounting (PipeCatalogueTest) counts it.
    inline void MGPipeRouteSetContextValues(const MGPContextValues& values) {
        MGP_SetContextValues(&values);
    }
    inline void MGPipeRouteSetVertexAttribDefaults(const MGPVertexAttribDefaults& hdr,
                                                   const MGPAttribValue* tail) {
        MGP_SetVertexAttribDefaults(&hdr, tail, hdr.Count);
    }
    // The block IS the blob (table 1 row 6) and its size is a header constant both sides
    // read, so no call site has to know it.
    inline void MGPipeRouteSetResidualValueState(const ResidualValueBlock& block) {
        MGPResidualValueState record{};
        record.Version = 0;
        MGP_SetResidualValueState(&record, &block, sizeof(ResidualValueBlock));
    }

    // ---- vertex input ------------------------------------------------------------------
    inline void MGPipeRouteCreateVertexElements(const MGPVertexElements& desc,
                                                const void* blobBytes) {
        MGP_CreateVertexElements(&desc, blobBytes, desc.Blob.Size);
    }
    inline void MGPipeRouteBindVertexElements(const MGPHandleOnly& handle) {
        MGP_BindVertexElements(&handle);
    }
    inline void MGPipeRouteDeleteVertexElements(const MGPHandleOnly& handle) {
        MGP_DeleteVertexElements(&handle);
    }
    inline void MGPipeRouteSetVertexBuffers(const MGPVertexBuffers& hdr,
                                            const MGPVertexBuffer* tail) {
        MGP_SetVertexBuffers(&hdr, tail, hdr.Count);
    }
    inline void MGPipeRouteSetIndexBuffer(const MGPIndexBuffer& record) {
        MGP_SetIndexBuffer(&record);
    }

    // ---- framebuffer, samplers, images -------------------------------------------------
    inline void MGPipeRouteSetFramebufferState(const MGPFramebufferState& state) {
        MGP_SetFramebufferState(&state);
    }
    // The parameters row declares Size 0 today and table 1 row 17 puts it under rule A, so
    // the length is stated here once, from the type the applier stores by value.
    inline void MGPipeRouteCreateSamplerState(const MGPSamplerDesc& desc,
                                              const SamplerParameters* parameters) {
        MGP_CreateSamplerState(&desc, parameters, sizeof(SamplerParameters));
    }
    inline void MGPipeRouteDeleteSamplerState(const MGPHandleOnly& handle) {
        MGP_DeleteSamplerState(&handle);
    }
    inline void MGPipeRouteCreateSamplerView(const MGPSamplerView& view) {
        MGP_CreateSamplerView(&view);
    }
    inline void MGPipeRouteDeleteSamplerView(const MGPHandleOnly& handle) {
        MGP_DeleteSamplerView(&handle);
    }
    inline Bool MGPipeRouteSetTextureParams(const MGPTextureParams& params) {
        MGPReplySlot reply = MGPipeMintReplySlot();
        MGP_SetTextureParams(&params, &reply);
        return MGPipeTakeReplyBool(reply, "set_texture_params");
    }
    inline void MGPipeRouteSetSamplerViews(const MGPSamplerViews& hdr, const MGPBoundView* tail) {
        MGP_SetSamplerViews(&hdr, tail, hdr.Count);
    }
    inline void MGPipeRouteBindSamplerStates(const MGPSamplerStates& hdr, const MGPipeHandle* tail) {
        MGP_BindSamplerStates(&hdr, tail, hdr.Count);
    }
    inline void MGPipeRouteSetShaderImages(const MGPShaderImages& hdr, const MGPImageView* tail) {
        MGP_SetShaderImages(&hdr, tail, hdr.Count);
    }

    // ---- indexed buffer binding points (P5e, sb) ----------------------------------------
    //
    // ONE ROUTE FOR THREE RECORDS: the class is a FIELD, not an opcode, so Uniform,
    // ShaderStorage and AtomicCounter all come through here and the applier keys on hdr.Class.
    // The second var-tail (MGHostSpan[HostSpanCount], D-B8) is never present on Espryt -
    // kCapNeedsHostUboBytes is 0 for the whole of P5 - so this wrapper states one tail, and the
    // codec's honesty pass is the guard that says so out loud if a backend ever asks for the
    // other one.
    inline void MGPipeRouteSetShaderBuffers(const MGPShaderBuffers& hdr, const MGPBufferRange* tail) {
        MGP_SetShaderBuffers(&hdr, tail, hdr.Count);
    }

    // ---- programs ----------------------------------------------------------------------
    inline void MGPipeRouteCreateShaderState(const MGPProgramDesc& desc,
                                             const MG_State::GLState::LinkArtifacts* link,
                                             const MG_State::GLState::SpirvArtifacts* spirv) {
        gMGPipeRouteEscapes.CreateShaderState(&desc, link, spirv);
    }
    inline void MGPipeRouteBindShaderState(const MGPHandleOnly& handle) {
        MGP_BindShaderState(&handle);
    }
    inline void MGPipeRouteDeleteShaderState(const MGPHandleOnly& handle) {
        MGP_DeleteShaderState(&handle);
    }
    inline void MGPipeRouteSetDrawProgram(const MGPHandleOnly& handle) {
        MGP_SetDrawProgram(&handle);
    }
    inline void MGPipeRouteSetDispatchProgram(const MGPHandleOnly& handle) {
        MGP_SetDispatchProgram(&handle);
    }
    inline void MGPipeRouteSetGlobalConstants(const MGPGlobalConstants& record, const void* bytes,
                                              Uint64 byteCount) {
        MGP_SetGlobalConstants(&record, bytes, byteCount);
    }

} // namespace MobileGL::MG_Pipe

#endif // MOBILEGL_PIPE_PUSH
