// MobileGL - MobileGL/MG_Remote/Client/WireTables.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The ENCODE TWIN of gMGPipeWireRecordApply: thirty-seven emitters that turn a table call into
// a wire record. Owner: package c1 (P5 ruling R-17). See WireTables.h for the install order
// and MG_Pipe/PipeRoute.h for what R-17 actually cost.
//
// EVERY EMITTER IS THE SAME FOUR STEPS, and the macros below exist so that a reader can check
// thirty-seven rows against PipeTables.inc in one pass instead of reading thirty-seven bodies:
//
//     1. require a session - a slot that fell through to a driver this role does not have is
//        the failure R-4 exists to prevent, and there is no fall-through here either;
//     2. stage the blob, if the row has one, and name the SEG_STAGE run in the payload's own
//        MGPBlobRef - which is why the payload is COPIED: the table hands it over const, and
//        the blobref is the one field the client must write after the caller is done with it;
//     3. EmitAndWait, which is the barrier's wait and the reply's wait at once (R-3/R-5);
//     4. post the answer, for the rows that have one, into MG_Pipe's reply mailbox.
//
// WHAT IS DELIBERATELY NOT HERE. b1's `PushPersistentMapsBeforeVerb` / `MarkGpuWritesFor*` are
// NOT called from these thirty-seven. They are pre-VERB hooks and these are not verbs: they
// are the resource, CSO and state records that a verb is later drawn against. The five class-B
// verbs in EmitTables.cpp call them, once each, immediately before their record, which is the
// ordering b1's B-1 fix depends on. Calling them here as well would push a persistent map
// before every `set_dynamic_state` - hundreds of times a frame, and each one a real record.

#include "WireTables.h"

#if MOBILEGL_BUILD_DISAGGREGATED

#include "ClientSession.h"

#include "../Server/ServerLoop.h"

#include <MG_Pipe/MGPipe.h>
#include <MG_Pipe/PipeRoute.h>
#include <MG_State/GLState/ProgramState/ProgramArtifactsCodec.h>
#include <MG_Util/Debug/Log.h>

#include <cstdlib>
#include <cstring>

namespace MobileGL::MG_Remote::Client {

    using MG_Pipe::MGPWireOp;

    namespace {

        // TABLE 3's ROLE SPLIT, AS A RUNTIME CHECK. gMGPipeScreen / gMGPipeContext are PROCESS
        // globals and under `inproc` the server role is a thread in this same process, so the
        // apply thread running the server's own backend - the EGL bring-up, InitCapabilities,
        // the applier - reaches these very emitters. A record published there would be waited
        // for by the thread that is supposed to apply it: `Fatal{BarrierTimeout,
        // "ResourceRespecify"}` from `mgl-srv-apply`, thirty seconds into bring-up, which is
        // exactly how this was found.
        //
        // THE ANSWER IS NOT "SUPPRESS THE RECORD" - it is "run the server's own code", because
        // on that thread this process IS the server and the applier is one call away. It is
        // the same thing PipeWireCodec does on the decode side, where every arm calls
        // MGPipeApply* directly and never goes through a table.
        //
        // Under `spawn` (P6) the predicate is constantly false in the client process and
        // constantly true in the server's, so this costs one atomic load and changes nothing.
        Bool RunsAsTheServerRole() { return Server::ServerLoop::OnApplyThread(); }

        Uint64 g_emitted = 0;
        Uint64 g_declined = 0;

        ClientSession& RequireSession(const char* row) {
            ClientSession* session = ClientSession::Active();
            if (session == nullptr) {
                MGLOG_F("MGPipe: Fatal{NoClientSession, \"%s\"} - the client wire tables are "
                        "installed but no ClientSession is active. A row may not fall through to "
                        "a driver this role does not have",
                        row);
                std::abort();
            }
            return *session;
        }

        // Stages a mandatory blob. `StageBytes` Fatals on a zero size by design (R-2.2: "the
        // record declared no blob" and "the record declared an empty blob" must not be spelled
        // the same way on a wire), so a row whose decoder calls RequireDeclaredBlob or
        // ResolveOrFatal is checked HERE, on the producing side, where the row has a name.
        MG_Pipe::MGPBlobRef StageRequired(ClientSession& session, const char* row,
                                          const void* bytes, Uint64 count) {
            if (bytes == nullptr || count == 0) {
                MGLOG_F("MGPipe: Fatal{BlobMissing, \"%s\"} - the row's decoder requires a "
                        "declared blob and the call site handed over %llu bytes at %p. Under "
                        "monolith the companion pointer carries them; under split they have to "
                        "be staged, and there is nothing to stage",
                        row, static_cast<unsigned long long>(count), bytes);
                std::abort();
            }
            return session.Encoder().StageBytes(bytes, count);
        }

        // Stages an OPTIONAL blob: the two sub-data rows, whose decoders resolve only when the
        // record's own size field says there are bytes. All three fields zero is the wire's
        // "no blob declared", and CheckBlobIsHonest refuses any other spelling of it.
        MG_Pipe::MGPBlobRef StageOptional(ClientSession& session, const void* bytes, Uint64 count) {
            if (bytes == nullptr || count == 0) return MG_Pipe::MGPBlobRef{};
            return session.Encoder().StageBytes(bytes, count);
        }

        // ---------------------------------------------------------------------------------
        // The three regular shapes
        // ---------------------------------------------------------------------------------

#define MGP_WIRE_PLAIN(Name, Payload, Table)                                                       \
    void Wire_##Name(const MG_Pipe::Payload* payload) {                                            \
        if (RunsAsTheServerRole()) { MG_Pipe::MGPipeMonolith##Table().Name(payload); return; }      \
        ClientSession& session = RequireSession(#Name);                                            \
        session.EmitAndWait(MGPWireOp::Name, payload, sizeof(*payload), nullptr, 0, nullptr, 0,    \
                            nullptr);                                                              \
        ++g_emitted;                                                                               \
    }

#define MGP_WIRE_BLOB(Name, Payload, BlobMember)                                                   \
    void Wire_##Name(const MG_Pipe::Payload* payload, const void* blobBytes,                       \
                     Uint64 blobByteCount) {                                                       \
        if (RunsAsTheServerRole()) {                                                               \
            MG_Pipe::MGPipeMonolithContext().Name(payload, blobBytes, blobByteCount);              \
            return;                                                                                \
        }                                                                                          \
        ClientSession& session = RequireSession(#Name);                                            \
        MG_Pipe::Payload record = *payload;                                                        \
        record.BlobMember = StageRequired(session, #Name, blobBytes, blobByteCount);               \
        session.EmitAndWait(MGPWireOp::Name, &record, sizeof(record), nullptr, 0, nullptr, 0,      \
                            nullptr);                                                              \
        ++g_emitted;                                                                               \
    }

#define MGP_WIRE_TAIL(Name, Payload, TailType)                                                     \
    void Wire_##Name(const MG_Pipe::Payload* payload, const void* varTail, Uint32 varTailCount) {   \
        if (RunsAsTheServerRole()) {                                                               \
            MG_Pipe::MGPipeMonolithContext().Name(payload, varTail, varTailCount);                 \
            return;                                                                                \
        }                                                                                          \
        ClientSession& session = RequireSession(#Name);                                            \
        session.EmitAndWait(MGPWireOp::Name, payload, sizeof(*payload), varTail,                   \
                            static_cast<Uint64>(varTailCount) * sizeof(MG_Pipe::TailType),         \
                            nullptr, 0, nullptr);                                                  \
        ++g_emitted;                                                                               \
    }

        // -- screen -------------------------------------------------------------------
        MGP_WIRE_PLAIN(ResourceDestroy, MGPHandleOnly, Screen)
        MGP_WIRE_PLAIN(UnmapPersistent, MGPHandleOnly, Screen)

        // -- context, plain -----------------------------------------------------------
        MGP_WIRE_PLAIN(BindRenderState, MGPBindRenderState, Context)
        MGP_WIRE_PLAIN(DeleteRenderState, MGPHandleOnly, Context)
        MGP_WIRE_PLAIN(BindVertexElements, MGPHandleOnly, Context)
        MGP_WIRE_PLAIN(DeleteVertexElements, MGPHandleOnly, Context)
        MGP_WIRE_PLAIN(DeleteSamplerState, MGPHandleOnly, Context)
        MGP_WIRE_PLAIN(CreateSamplerView, MGPSamplerView, Context)
        MGP_WIRE_PLAIN(DeleteSamplerView, MGPHandleOnly, Context)
        MGP_WIRE_PLAIN(BindShaderState, MGPHandleOnly, Context)
        MGP_WIRE_PLAIN(DeleteShaderState, MGPHandleOnly, Context)
        MGP_WIRE_PLAIN(SetDrawProgram, MGPHandleOnly, Context)
        MGP_WIRE_PLAIN(SetDispatchProgram, MGPHandleOnly, Context)
        MGP_WIRE_PLAIN(SetFramebufferState, MGPFramebufferState, Context)
        MGP_WIRE_PLAIN(SetIndexBuffer, MGPIndexBuffer, Context)
        MGP_WIRE_PLAIN(SetPixelPackState, MGPPixelPackState, Context)
        MGP_WIRE_PLAIN(SetPatchState, MGPPatchState, Context)

        // -- context, mandatory blob --------------------------------------------------
        MGP_WIRE_BLOB(CreateRenderState, MGPRenderStateDesc, Blob)
        MGP_WIRE_BLOB(CreateVertexElements, MGPVertexElements, Blob)
        MGP_WIRE_BLOB(CreateSamplerState, MGPSamplerDesc, Parameters)
        MGP_WIRE_BLOB(SetDynamicState, MGPDynamicState, Blob)
        MGP_WIRE_BLOB(SetGlobalConstants, MGPGlobalConstants, Blob)

        // -- context, variable tail ---------------------------------------------------
        MGP_WIRE_TAIL(SetVertexBuffers, MGPVertexBuffers, MGPVertexBuffer)
        MGP_WIRE_TAIL(SetSamplerViews, MGPSamplerViews, MGPBoundView)
        MGP_WIRE_TAIL(BindSamplerStates, MGPSamplerStates, MGPipeHandle)
        MGP_WIRE_TAIL(SetShaderImages, MGPShaderImages, MGPImageView)
        MGP_WIRE_TAIL(SetVertexAttribDefaults, MGPVertexAttribDefaults, MGPAttribValue)

#undef MGP_WIRE_PLAIN
#undef MGP_WIRE_BLOB
#undef MGP_WIRE_TAIL

        // -- the rows that fit none of the three shapes --------------------------------

        // set_residual_value_state. CONTRACT-P5 table 1 row 6: the applier takes a frontend
        // `ResidualValueBlock&` and `MGPResidualValueState` is never instantiated on the live
        // path, so the encoder invents BOTH the record fill and the blob fill. The block IS
        // the blob, whole - the decoder requires exactly sizeof(ResidualValueBlock) and says
        // why ("a size that only ever ratchets down makes a short read silently lose
        // CapabilityBits"), so the two sides state the same number from the same header.
        void Wire_SetResidualValueState(const MG_Pipe::MGPResidualValueState* payload,
                                        const void* blobBytes, Uint64 blobByteCount) {
            if (RunsAsTheServerRole()) {
                MG_Pipe::MGPipeMonolithContext().SetResidualValueState(payload, blobBytes,
                                                                       blobByteCount);
                return;
            }
            ClientSession& session = RequireSession("SetResidualValueState");
            MG_Pipe::MGPResidualValueState record = *payload;
            record.Blob = StageRequired(session, "SetResidualValueState", blobBytes, blobByteCount);
            session.EmitAndWait(MGPWireOp::SetResidualValueState, &record, sizeof(record), nullptr,
                                0, nullptr, 0, nullptr);
            ++g_emitted;
        }

        // resource_readback. kReplySlot, and the answer is COMPLETION only: the bytes travel
        // server -> client in SEG_EVENT through OnBufferWriteback, because the destination is
        // the client's shadow and its size is the resource's, not a slot's (CONTRACT-P5 table 1
        // row 22). So the reply buffer is deliberately {nullptr, 0} and the wait is what makes
        // the writeback already drained by the time this returns.
        void Wire_ResourceReadback(const MG_Pipe::MGPReadback* payload, MG_Pipe::MGPReplySlot* reply) {
            if (RunsAsTheServerRole()) { MG_Pipe::MGPipeMonolithContext().ResourceReadback(payload, reply); return; }
            ClientSession& session = RequireSession("ResourceReadback");
            Int32 status = 0;
            const Uint64 seq = session.EmitAndWait(MGPWireOp::ResourceReadback, payload,
                                                   sizeof(*payload), nullptr, 0, nullptr, 0,
                                                   &status);
            reply->Id = seq;
            MG_Pipe::MGPipePostReply(*reply, status, 0);
            ++g_emitted;
        }

        // ---- the three acceptance rows that fit a generated signature ----------------
        //
        // THE ANSWER IS THE SERVER'S AND NOTHING ELSE. `EmitAndWait` returns the record's seq
        // and fills `status` from the reply slot the server stamped; DECLINED is `false` and OK
        // is `true`, and neither is derived from anything this side knows. R-5 exists because
        // "always accept" is ID-39's 66 lost DirectVulkan uploads and "accept if we emitted" is
        // the same bug wearing a counter.

        void Wire_ResourceCreate(const MG_Pipe::MGPResourceDesc* payload, MG_Pipe::MGPReplySlot* reply) {
            if (RunsAsTheServerRole()) { MG_Pipe::MGPipeMonolithScreen().ResourceCreate(payload, reply); return; }
            ClientSession& session = RequireSession("ResourceCreate");
            Int32 status = 0;
            const Uint64 seq = session.EmitAndWait(MGPWireOp::ResourceCreate, payload,
                                                   sizeof(*payload), nullptr, 0, nullptr, 0,
                                                   &status);
            reply->Id = seq;
            MG_Pipe::MGPipePostReply(*reply, status, status == 0 ? 1u : 0u);
            ++g_emitted;
            if (status == 1) ++g_declined;
        }

        void Wire_SetTextureParams(const MG_Pipe::MGPTextureParams* payload, MG_Pipe::MGPReplySlot* reply) {
            if (RunsAsTheServerRole()) { MG_Pipe::MGPipeMonolithContext().SetTextureParams(payload, reply); return; }
            ClientSession& session = RequireSession("SetTextureParams");
            Int32 status = 0;
            const Uint64 seq = session.EmitAndWait(MGPWireOp::SetTextureParams, payload,
                                                   sizeof(*payload), nullptr, 0, nullptr, 0,
                                                   &status);
            reply->Id = seq;
            MG_Pipe::MGPipePostReply(*reply, status, status == 0 ? 1u : 0u);
            ++g_emitted;
            if (status == 1) ++g_declined;
        }

        void Wire_ResourceSubData(const MG_Pipe::MGPSubData* payload, const void* blobBytes,
                                  Uint64 blobByteCount, const void* varTail, Uint32 varTailCount,
                                  MG_Pipe::MGPReplySlot* reply) {
            if (RunsAsTheServerRole()) {
                MG_Pipe::MGPipeMonolithContext().ResourceSubData(payload, blobBytes, blobByteCount,
                                                                 varTail, varTailCount, reply);
                return;
            }
            ClientSession& session = RequireSession("ResourceSubData");
            MG_Pipe::MGPSubData record = *payload;
            record.Blob = StageOptional(session, blobBytes, blobByteCount);
            Int32 status = 0;
            const Uint64 seq = session.EmitAndWait(
                MGPWireOp::ResourceSubData, &record, sizeof(record), varTail,
                static_cast<Uint64>(varTailCount) * sizeof(MG_Pipe::MGPSubRegion), nullptr, 0,
                &status);
            reply->Id = seq;
            MG_Pipe::MGPipePostReply(*reply, status, status == 0 ? 1u : 0u);
            ++g_emitted;
            if (status == 1) ++g_declined;
        }

        void Wire_BufferSubDataResident(const MG_Pipe::MGPSubData* payload, const void* blobBytes,
                                        Uint64 blobByteCount) {
            if (RunsAsTheServerRole()) {
                MG_Pipe::MGPipeMonolithContext().BufferSubDataResident(payload, blobBytes,
                                                                       blobByteCount);
                return;
            }
            ClientSession& session = RequireSession("BufferSubDataResident");
            MG_Pipe::MGPSubData record = *payload;
            record.Blob = StageOptional(session, blobBytes, blobByteCount);
            session.EmitAndWait(MGPWireOp::BufferSubDataResident, &record, sizeof(record), nullptr,
                                0, nullptr, 0, nullptr);
            ++g_emitted;
        }

        // ---- the four escapes --------------------------------------------------------

        // resource_respecify. R-13.3: `initialBytes` is ALWAYS nullptr under split and the
        // initial content arrives as resource_subdata records immediately after this one. The
        // caller's bytes are therefore not dropped - they are re-expressed - and PipeFill.cpp's
        // emitter is where that happens, because the chunking walk that has to size them
        // (MGPipeForEachSubDataRecordRange) lives there. What this emitter owes is the REFUSAL:
        // a non-null pointer arriving here means the call site was not converted, and silently
        // ignoring it would lose exactly the bytes R-13.3 promised would follow.
        Bool Wire_Escape_ResourceRespecify(const MG_Pipe::MGPResourceDesc* desc,
                                           const void* initialBytes,
                                           const MG_Pipe::MGPRespecifiedLevel* level) {
            if (RunsAsTheServerRole()) {
                return MG_Pipe::MGPipeMonolithEscapes().ResourceRespecify(desc, initialBytes, level);
            }
            ClientSession& session = RequireSession("ResourceRespecify");
            if (initialBytes != nullptr) {
                MGLOG_F("MGPipe: Fatal{UncarriedInitialBytes, \"resource_respecify\"} - a call "
                        "site handed initial content to a split respecify. R-13.3 rules that "
                        "initialBytes never crosses and that the content follows as "
                        "resource_subdata; a caller that still passes it has bytes nothing will "
                        "carry");
                std::abort();
            }
            // The scope rides in the descriptor's own pads (CONTRACT-P5 table 1 row 19b, LANDED)
            // and is written only through MGPipeSetRespecifiedLevel - three fields are one
            // value, and an open-coded writer that forgets the presence byte says "level 0 of
            // upload target 0" where it meant "the whole resource".
            MG_Pipe::MGPResourceDesc record = *desc;
            if (level != nullptr) {
                MG_Pipe::MGPipeSetRespecifiedLevel(record, level->UploadTarget, level->Level);
            } else {
                MG_Pipe::MGPipeClearRespecifiedLevel(record);
            }
            Int32 status = 0;
            const Uint64 seq =
                session.EmitAndWait(MGPWireOp::ResourceRespecify, &record, sizeof(record), nullptr,
                                    0, nullptr, 0, &status);
            (void)seq;
            ++g_emitted;
            if (status == 1) ++g_declined;
            return status == 0;
        }

        // resource_flush_range. R-13.2: it carries NO bytes under split - it is a
        // {range, AccessFlags} control record and the bytes of exactly that range arrive ahead
        // of it as resource_subdata. Same refusal as above, for the same reason: a blobref here
        // would be "a second, forgeable way to say the same thing".
        void Wire_Escape_ResourceFlushRange(const MG_Pipe::MGPFlushRange* record,
                                            const void* bytes) {
            if (RunsAsTheServerRole()) {
                MG_Pipe::MGPipeMonolithEscapes().ResourceFlushRange(record, bytes);
                return;
            }
            ClientSession& session = RequireSession("ResourceFlushRange");
            (void)bytes; // ruled uncarried; the emitter in PipeFill.cpp sends the range first
            session.EmitAndWait(MGPWireOp::ResourceFlushRange, record, sizeof(*record), nullptr, 0,
                                nullptr, 0, nullptr);
            ++g_emitted;
        }

        // map_persistent. R-6/R-2.4: the split answer is a CONSTANT DECLINE, and it still costs
        // a record, because the server has to know the client asked - the applier's
        // MapPersistentRoundtrips counter is defined as "one per storage definition in both
        // modes" and a client that answered locally would zero it. `size` and `seedBytes` have
        // no carrier (MGPHandleOnly is {Handle, Kind}) and need none: nothing is minted.
        void* Wire_Escape_MapPersistent(const MG_Pipe::MGPHandleOnly* handle, Uint64 size,
                                        const void* seedBytes) {
            if (RunsAsTheServerRole()) {
                return MG_Pipe::MGPipeMonolithEscapes().MapPersistent(handle, size, seedBytes);
            }
            ClientSession& session = RequireSession("MapPersistent");
            (void)size;
            (void)seedBytes;
            Int32 status = 0;
            session.EmitAndWait(MGPWireOp::MapPersistent, handle, sizeof(*handle), nullptr, 0,
                                nullptr, 0, &status);
            ++g_emitted;
            if (status == 1) ++g_declined;
            // NOT "always nullptr": the answer is READ. R-6 says the server declines, and the
            // day it stops declining this returns what it actually said rather than what the
            // ruling predicted.
            if (status == 0) {
                MGLOG_F("MGPipe: Fatal{UnexpectedMapAccept, \"map_persistent\"} - the server "
                        "accepted a persistent map under split. R-6 makes the split answer a "
                        "constant decline because there is no way to hand a host pointer across "
                        "a process boundary in P5; a pointer arriving here is one this client "
                        "cannot dereference");
                std::abort();
            }
            return nullptr;
        }

        // create_shader_state. SEVEN blobrefs and TWO typed frontend pointers; one blobBytes
        // pair cannot express seven runs. The serializer already exists and no package may
        // write a second one (CONTRACT-P5 table 1 row 3): EncodeProgramArtifacts produces one
        // archive, the decoder's DecodeProgramArtifacts consumes it, and the six per-stage runs
        // stay UNDECLARED because the modules already travel inside the archive - a declared
        // Spirv[i] is Fatal on the far side rather than ignored.
        void Wire_Escape_CreateShaderState(const MG_Pipe::MGPProgramDesc* desc,
                                           const MG_State::GLState::LinkArtifacts* link,
                                           const MG_State::GLState::SpirvArtifacts* spirv) {
            if (RunsAsTheServerRole()) {
                MG_Pipe::MGPipeMonolithEscapes().CreateShaderState(desc, link, spirv);
                return;
            }
            ClientSession& session = RequireSession("CreateShaderState");
            if (link == nullptr || spirv == nullptr) {
                MGLOG_F("MGPipe: Fatal{ArtefactsMissing, \"create_shader_state\"} - the record's "
                        "two typed companions are null. Under monolith the applier reads the "
                        "modules out of spirv->generatedSpirv; under split there is nothing to "
                        "serialise, and emitting the record anyway would create a CSO with no "
                        "code");
                std::abort();
            }
            // EncodeProgramArtifacts APPENDS and never fails - everything it walks is owned
            // plain data - so an empty archive means the two structs themselves were empty,
            // which is a linked program with no artefacts and is not a codec question.
            Vector<Uint8> archive;
            MG_State::GLState::EncodeProgramArtifacts(*link, *spirv, archive);
            if (archive.empty()) {
                MGLOG_F("MGPipe: Fatal{ArchiveEmpty, \"create_shader_state\"} - the program's "
                        "artefacts serialised to nothing");
                std::abort();
            }
            MG_Pipe::MGPProgramDesc record = *desc;
            for (Uint32 i = 0; i < 6; ++i) record.Spirv[i] = MG_Pipe::MGPBlobRef{};
            record.Reflection = session.Encoder().StageBytes(archive.data(), archive.size());
            session.EmitAndWait(MGPWireOp::CreateShaderState, &record, sizeof(record), nullptr, 0,
                                nullptr, 0, nullptr);
            ++g_emitted;
        }

    } // namespace

    void InstallClientWireTables() {
        using namespace MG_Pipe;

        gMGPipeScreen.ResourceCreate = &Wire_ResourceCreate;
        gMGPipeScreen.ResourceDestroy = &Wire_ResourceDestroy;
        gMGPipeScreen.UnmapPersistent = &Wire_UnmapPersistent;

        gMGPipeContext.CreateRenderState = &Wire_CreateRenderState;
        gMGPipeContext.BindRenderState = &Wire_BindRenderState;
        gMGPipeContext.DeleteRenderState = &Wire_DeleteRenderState;
        gMGPipeContext.CreateVertexElements = &Wire_CreateVertexElements;
        gMGPipeContext.BindVertexElements = &Wire_BindVertexElements;
        gMGPipeContext.DeleteVertexElements = &Wire_DeleteVertexElements;
        gMGPipeContext.CreateSamplerState = &Wire_CreateSamplerState;
        gMGPipeContext.DeleteSamplerState = &Wire_DeleteSamplerState;
        gMGPipeContext.CreateSamplerView = &Wire_CreateSamplerView;
        gMGPipeContext.DeleteSamplerView = &Wire_DeleteSamplerView;
        gMGPipeContext.BindShaderState = &Wire_BindShaderState;
        gMGPipeContext.DeleteShaderState = &Wire_DeleteShaderState;
        gMGPipeContext.SetDrawProgram = &Wire_SetDrawProgram;
        gMGPipeContext.SetDispatchProgram = &Wire_SetDispatchProgram;
        gMGPipeContext.SetDynamicState = &Wire_SetDynamicState;
        gMGPipeContext.SetFramebufferState = &Wire_SetFramebufferState;
        gMGPipeContext.SetVertexBuffers = &Wire_SetVertexBuffers;
        gMGPipeContext.SetIndexBuffer = &Wire_SetIndexBuffer;
        gMGPipeContext.SetSamplerViews = &Wire_SetSamplerViews;
        gMGPipeContext.BindSamplerStates = &Wire_BindSamplerStates;
        gMGPipeContext.SetShaderImages = &Wire_SetShaderImages;
        gMGPipeContext.SetGlobalConstants = &Wire_SetGlobalConstants;
        gMGPipeContext.SetVertexAttribDefaults = &Wire_SetVertexAttribDefaults;
        gMGPipeContext.SetPixelPackState = &Wire_SetPixelPackState;
        gMGPipeContext.SetPatchState = &Wire_SetPatchState;
        gMGPipeContext.SetResidualValueState = &Wire_SetResidualValueState;
        gMGPipeContext.SetTextureParams = &Wire_SetTextureParams;
        gMGPipeContext.ResourceSubData = &Wire_ResourceSubData;
        gMGPipeContext.BufferSubDataResident = &Wire_BufferSubDataResident;
        gMGPipeContext.ResourceReadback = &Wire_ResourceReadback;

        gMGPipeRouteEscapes.ResourceRespecify = &Wire_Escape_ResourceRespecify;
        gMGPipeRouteEscapes.ResourceFlushRange = &Wire_Escape_ResourceFlushRange;
        gMGPipeRouteEscapes.MapPersistent = &Wire_Escape_MapPersistent;
        gMGPipeRouteEscapes.CreateShaderState = &Wire_Escape_CreateShaderState;

        MGPipeNoteInstalledArm(MGPipeRouteArm::kClientWire);
    }

    void UninstallClientWireTables() {
        // PUTS THE MONOLITH ARM BACK rather than nulling the rows. A null row is the
        // pre-migration state and would be an immediate crash with no diagnostic at whatever
        // GL call raced the teardown; the monolith adapter is at least a correct answer for a
        // process that no longer has a session.
        MG_Pipe::MGPipeInstallMonolithTables();
    }

    Uint64 ClientWireRecordsEmitted() { return g_emitted; }
    Uint64 ClientWireRecordsDeclined() { return g_declined; }

} // namespace MobileGL::MG_Remote::Client

#endif // MOBILEGL_BUILD_DISAGGREGATED
