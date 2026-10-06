// MobileGL - MobileGL/MG_Impl/Pipe/Verb/VerbPort.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P13 W5: THE VERB EMITTERS AND THE MONOLITH VERB PORT, moved from MG_Remote/Client/EmitTables.cpp
// so a library without a transport runs the record arm. One planner per verb serves both shapes:
// under a transport its record goes to the session (MG_Remote resolves an MG_Record::VerbSession),
// and on monolith's record arm to an in-process RecordVerbSink bound to the one backend object.

#include "VerbPort.h"
#include "GpuWriteSet.h"

#include <MG_Backend/BackendObjects.h>
#include <MG_Backend/Record/RecordVerbSink.h>
#include <MG_Pipe/PipeClientSeam.h>
#include <MG_Pipe/PipeSessionFail.h>
#include <MG_Pipe/PipeVerbSink.h>
#include <MG_State/GLState/BufferState/PersistentMapTracker.h>

#include <MG_Util/Converters/GLToMG/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToGL/TextureEnumConverter.h>
#include <MG_Util/Debug/Log.h>
#include <MG_Util/Metrics/PipeStats.h>
#include <MG_Util/Metrics/TextureMetrics.h>

#include <MG_State/GLState/BufferState/BufferObject.h>
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/VertexArrayState/VertexArrayObject.h>
#include <MG_Impl/Pipe/SlotAllocator.h>
#include <MG_State/GLState/ProgramState/ProgramObject.h>
#include <MG_State/GLState/TextureState/TextureState.h>
#include <MG_Impl/Pipe/ResourceTracker.h>
#include <MG_Impl/GLImpl/Texture/MipmapGenerationPlan.h>
#include <MG_Impl/Pipe/OwnedDrawInputs.h>
#include <MG_Impl/Pipe/FramebufferEmit.h>
#include <MG_Impl/Pipe/PipeFill.h>
#include <MG_Impl/Pipe/TextureEmit.h>
#include <MG_Impl/Pipe/ProgramEmit.h>
#include <MG_Pipe/PipeMutation.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace MobileGL::MG_Record {

    [[noreturn]] void UnmigratedVerbFatal(const char* slot) {
        // The same shape as MGPipeInputPoisonFatal (generated/PipeFilled.inc:407-413): names the
        // slot, live at every log level, aborts. Deliberately NOT MOBILEGL_ASSERT, which is
        // inert in an INFO build - and INFO is what every device lane runs.
        MG_Pipe::MGPipeRecordFail(MG_Pipe::MGFatalFamily::UnmigratedVerb, "MGPipe: Fatal{UnmigratedVerb, \"%s\"}", slot);
    }

    // A verb that reads buffers but starts no shader: clear, blit, readback, present. The
    // push still has to run - a coherent map is read by the GPU on any of them - but there
    // is no shader that could write one, so no mark walk.
    void BeforeReadOnlyVerb() {
        MG_Record::PushPersistentMapsBeforeVerb();
        MG_Pipe::MGPipeDrainDeferredDestroys();
    }

    namespace {

        Bool g_dropClearEmission = false;
        Uint64 g_droppedClearEmissions = 0;
        Bool g_dropDrawEmission = false;
        Uint64 g_droppedDrawEmissions = 0;

        // E2's control has to be armable from OUTSIDE the process that runs the replay, because
        // the statement it makes is about a trace lane and not about a unit case: "drop an
        // emission and OpenRA's SSIM falls below 0.99". A recompile would make the control
        // arm against source text, which is ID-22(a)'s defect.
        //
        // READ WITH getenv RATHER THAN THROUGH MG_Config, DELIBERATELY AND TEMPORARILY. Config.h
        // is c0's and a new MOBILEGL_IPC_* knob goes through the integrator; these are
        // NEGATIVE-CONTROL switches no operator may ever set, and they announce themselves at
        // warning level every time they arm so they cannot be on by accident. Flagged for
        // adoption into IpcTable if the integrator wants them there.
        Bool ReadControlKnob(const char* name) {
            const char* value = std::getenv(name);
            return value != nullptr && value[0] == '1' && value[1] == '\0';
        }

        // ---- WHY THERE ARE TWO OF THESE KNOBS, measured rather than assumed -----------------
        //
        // MOBILEGL_IPC_E2_DROP_CLEAR came first and it does exactly what it says: every glClear
        // stops at the client and no Clear record reaches the ring. It STILL COULD NOT TURN THE
        // E2 RETRACE RED (joint-v1.md 3: SSIM 1.000000, mismatchPixels=0 with the knob armed and
        // the arming WARN in the library's own log). That is not a broken knob, it is OpenRA:
        // `apitrace dump` of openra.trace over the 31249 replayed calls counts 30 glClear, 30
        // glXSwapBuffers and 788 glDrawArrays, and the final frame issues its clear at call
        // 30197 and then covers the surface four times over with a terrain layer
        // (`glDrawArrays(GL_TRIANGLES, first=56064, count=16128)` x4, under a scissor of
        // -24,-24,688x528 over a 640x480 surface) before the snapshot at 31249. A frame that
        // overdraws every pixel it clears has a picture that does not depend on the clear, so
        // "drop the clear" is a control whose observable is invisible to THIS trace - R-16's
        // exact defect, a gate that cannot go red for its own reason.
        //
        // MOBILEGL_IPC_E2_DROP_DRAW is the honest form of the same statement for a trace lane:
        // drop every DrawVbo record and the picture can only be the clear colour. It is the
        // control that makes "the wire carried the frame" falsifiable, because the thing it
        // removes is the thing the golden is made of.
        //
        // DROP_CLEAR IS KEPT rather than retired: it drops a real record, it now publishes the
        // count it dropped (below), and a scenario whose picture DOES depend on its clear -
        // ClearThenReadPixelsScenario is the reduced path's target A - is where it is
        // observable. What it is no longer allowed to be is E2's retrace control.
        void ArmControlKnobs() {
            g_dropClearEmission = ReadControlKnob("MOBILEGL_IPC_E2_DROP_CLEAR");
            g_dropDrawEmission = ReadControlKnob("MOBILEGL_IPC_E2_DROP_DRAW");
            if (g_dropClearEmission) {
                MGLOG_W("MG_Remote client: MOBILEGL_IPC_E2_DROP_CLEAR=1 - a NEGATIVE CONTROL is "
                        "armed and every glClear will be DROPPED on the wire. It is observable "
                        "only where the picture depends on the clear: OpenRA overdraws its whole "
                        "surface every frame, so this knob does NOT redden the E2 retrace "
                        "(measured, joint-v1.md 3) - MOBILEGL_IPC_E2_DROP_DRAW is the one that "
                        "does. The dropped count is published on the 'E2 control armed' line");
            }
            if (g_dropDrawEmission) {
                MGLOG_W("MG_Remote client: MOBILEGL_IPC_E2_DROP_DRAW=1 - E2's NEGATIVE CONTROL is "
                        "armed and every DrawVbo record will be DROPPED on the wire. The surface "
                        "can then only carry the clear colour, so this arm is expected to fail its "
                        "SSIM threshold; a lane that stays green with it set is not going through "
                        "the wire at all");
            }
        }

        // ID-49's two halves, in one place so the emitter and its control read the same
        // arithmetic.
        //
        // GL 4.6 8.4.4, pack side: the destination row stride is ROW_LENGTH (or the width)
        // pixels rounded UP to PACK_ALIGNMENT, the first written byte is offset by SKIP_ROWS
        // whole strides plus SKIP_PIXELS pixels, and only `width * bytesPerPixel` bytes of each
        // stride are written - the gaps belong to the application and are never touched. That
        // last clause is what the control checks with a sentinel.
        Bool ReadbackPackStateIsTight(GLsizei width, Uint64 bytesPerPixel,
                                      const PixelStoreParameters& pack) {
            // SKIP_IMAGES (and IMAGE_HEIGHT) are NOT consulted: glReadPixels is a 2-D read and GL
            // ignores the image-level pack parameters for it, exactly as the monolith conversion
            // path does at DirectGLES.cpp:10905 (honorPackImageParams=false). A non-zero
            // SkipImages therefore does not make the layout non-tight (codex 6).
            if (pack.SkipRows != 0 || pack.SkipPixels != 0) return false;
            if (pack.RowLength != 0 && pack.RowLength != width) return false;
            const Uint64 alignment = pack.Alignment > 0 ? static_cast<Uint64>(pack.Alignment) : 1ull;
            const Uint64 rowBytes = static_cast<Uint64>(width) * bytesPerPixel;
            return (rowBytes % alignment) == 0;
        }

        // A band's box origin (g5-readback): the read's origin plus the band's offset, summed
        // wide. It can only leave Int32 for a read whose far edge was already past INT32_MAX,
        // and every pixel out there is outside any framebuffer - GL leaves their values
        // undefined - so it saturates instead of wrapping onto real pixels.
        Int32 ReadbackBandOrigin(GLint origin, Uint64 offset) {
            const Int64 at = static_cast<Int64>(origin) + static_cast<Int64>(offset);
            constexpr Int64 kMax = std::numeric_limits<Int32>::max();
            return static_cast<Int32>(at > kMax ? kMax : at);
        }

        // ---- P13 W5: where a split emitter's record goes ----------------------------------
        //
        // The session, resolved by MG_Remote (EmitTables.cpp installs the resolver at static init).
        // A library without a transport has none: there, only the monolith verb port reaches an
        // emitter, and an emitter reached without it would be a slot falling through to nothing.
        VerbSessionResolver g_verbSessionResolver = nullptr;

        VerbSession& RequireVerbSession(const char* slot) {
            if (g_verbSessionResolver == nullptr) {
                MG_Pipe::MGPipeRecordFail(MG_Pipe::MGFatalFamily::NoClientSession,
                                          "MGPipe: Fatal{NoClientSession, \"%s\"} - a verb emitter ran with neither "
                                          "the monolith verb port installed nor a transport to carry its record",
                                          slot);
            }
            return g_verbSessionResolver(slot);
        }


        // ---- P13 W4: THE MONOLITH VERB PORT ----------------------------------------------
        //
        // Monolith's record arm (MG_Config::RecordArmAliasesFrontend) reads the verb's own state -
        // VerbIndirectBuffer, VerbDispatchIndirectBuffer, ... - and nothing in monolith wrote it:
        // ServerVerbSink is its only writer, and monolith never decoded a verb record. So the
        // ported slots run THESE emitters - the same plan, the same pre-verb hooks in the same
        // order (persistent-map push, owned client-memory inputs, the GPU-write marks) - and hand
        // the planned record, in this process and on this thread, to a ServerVerbSink bound to
        // the one backend object, which stamps the verb state exactly as the apply thread does
        // and calls the backend's REAL table (ServerVerbSink::Table, never gBackendFunctionsTable,
        // so nothing re-enters this port). One planner and one sink body serve both shapes: the
        // monolith record arm is the split record arm minus the codec and the ring.
        Bool g_monolithVerbPort = false;

        RecordVerbSink& MonolithVerbSink() {
            // Leaked at exit like every other process-lifetime singleton (ID-8).
            static RecordVerbSink& sink = *new RecordVerbSink();
            return sink;
        }

        // The answer of a reply row on the port: written straight into the caller's buffer, which
        // is what the session's reply slot would have been copied into.
        class MonolithReplySink final : public MG_Pipe::MGPipeReplySink {
        public:
            MonolithReplySink(void* out, Uint64 capacity) : m_out(out), m_capacity(capacity) {}
            void PostReply(Uint64, Int32 status, const void* bytes, Uint64 size) override {
                m_posted = true;
                m_status = status;
                m_size = size;
                if (status == kStatusOk && bytes != nullptr && m_out != nullptr && size != 0) {
                    std::memcpy(m_out, bytes, static_cast<SizeT>(std::min(size, m_capacity)));
                }
            }
            Bool Posted() const { return m_posted; }
            Int32 Status() const { return m_status; }
            Uint64 Size() const { return m_size; }

        private:
            void* m_out;
            Uint64 m_capacity;
            Bool m_posted = false;
            Int32 m_status = kStatusError;
            Uint64 m_size = 0;
        };

        // The port's reply cap: one band answers any read a process can hold (PlanReadbackBands
        // still cuts a row wider than this, as it would for a slot).
        constexpr Uint32 kMonolithMaxReplyBytes = 0x7fffffffu;

#if MOBILEGL_PIPE_VERIFY
        thread_local Bool t_portApplying = false;
        struct PortApplyingScope {
            PortApplyingScope() { t_portApplying = true; }
            ~PortApplyingScope() { t_portApplying = false; }
        };
#endif

        Bool ApplyOnMonolithPort(MG_Pipe::MGPWireOp op, const void* payload, const MG_Pipe::MGPipeVerbTail* tails,
                                 Uint32 tailCount, MG_Pipe::MGPipeReplySink* replies) {
#if MOBILEGL_PIPE_VERIFY
            const PortApplyingScope applying;
#endif
            RecordVerbSink& sink = MonolithVerbSink();
            MG_Backend::BackendObject* backend = MG_Backend::pActiveBackendObject.get();
            if (sink.Backend() != backend) {
                sink.SetBackend(backend);
                sink.SetMaxReplyBytes(kMonolithMaxReplyBytes);
            }
            using Op = MG_Pipe::MGPWireOp;
            switch (op) {
            case Op::DrawVbo: {
                const auto& info = *static_cast<const MG_Pipe::MGPDrawInfo*>(payload);
                const auto* ranges = (tailCount > 0 && tails[0].Size != 0)
                                         ? static_cast<const MG_Pipe::MGPDrawRange*>(tails[0].Bytes)
                                         : nullptr;
                const auto* indirect = ((info.Flags & MG_Pipe::kDrawIsIndirect) != 0 && tailCount > 1)
                                           ? static_cast<const MG_Pipe::MGPDrawIndirect*>(tails[1].Bytes)
                                           : nullptr;
                return sink.OnDrawVbo(info, ranges, nullptr, indirect);
            }
            case Op::LaunchGrid:
                return sink.OnLaunchGrid(*static_cast<const MG_Pipe::MGPGridInfo*>(payload));
            // ---- W4b: the texture family's verbs ----
            case Op::GenerateMipmap:
                return sink.OnGenerateMipmap(*static_cast<const MG_Pipe::MGPMipPlan*>(payload));
            case Op::CopyFramebufferToTexture:
                return sink.OnCopyFramebufferToTexture(*static_cast<const MG_Pipe::MGPCopyFromFramebuffer*>(payload));
            case Op::ResourceCopyRegion:
                return sink.OnResourceCopyRegion(*static_cast<const MG_Pipe::MGPCopyRegion*>(payload));
            // ---- W4b: the readbacks. The reply rows answer through `replies`, which writes the
            // caller's buffer; the pack-buffer rows land in the buffer through the applier.
            case Op::ReadPixels:
                return sink.OnReadPixels(*static_cast<const MG_Pipe::MGPReadbackInfo*>(payload), 0, replies);
            case Op::GetTextureImage:
                return sink.OnGetTextureImage(*static_cast<const MG_Pipe::MGPReadbackInfo*>(payload), 0, replies);
            case Op::ReadPixelsToBuffer:
                return sink.OnReadPixelsToBuffer(*static_cast<const MG_Pipe::MGPReadbackToBuffer*>(payload));
            case Op::GetTextureImageToBuffer:
                return sink.OnGetTextureImageToBuffer(*static_cast<const MG_Pipe::MGPReadbackToBuffer*>(payload));
            // ---- W4c: framebuffers, image units, storage-block bindings ----
            case Op::Clear:
                return sink.OnClear(*static_cast<const MG_Pipe::MGPClear*>(payload));
            case Op::Blit:
                return sink.OnBlit(*static_cast<const MG_Pipe::MGPBlit*>(payload));
            case Op::BindShaderImage:
                return sink.OnBindShaderImage(*static_cast<const MG_Pipe::MGPImageBind*>(payload));
            // ---- W4a's XFB half: the capture span and its object, by lifetime id ----
            case Op::BeginStreamOutput:
                return sink.OnBeginStreamOutput(*static_cast<const MG_Pipe::MGPStreamOutputBegin*>(payload));
            case Op::EndStreamOutput:
                return sink.OnEndStreamOutput(*static_cast<const MG_Pipe::MGPXfbAccounting*>(payload));
            case Op::PauseStreamOutput:
                return sink.OnPauseStreamOutput(*static_cast<const MG_Pipe::MGPStreamOutputControl*>(payload));
            case Op::ResumeStreamOutput:
                return sink.OnResumeStreamOutput(*static_cast<const MG_Pipe::MGPStreamOutputControl*>(payload));
            case Op::BindStreamOutput:
                return sink.OnBindStreamOutput(*static_cast<const MG_Pipe::MGPStreamOutputBind*>(payload));
            case Op::DeleteStreamOutput:
                return sink.OnDeleteStreamOutput(*static_cast<const MG_Pipe::MGPStreamOutputBind*>(payload));
            case Op::SetStorageBlockBinding: {
                // The name is the emitter's own NUL-terminated string (VerbChannel::StageBytes on
                // the port names it by address), alive for this call.
                const auto& record = *static_cast<const MG_Pipe::MGPStorageBlockBinding*>(payload);
                const auto* name = reinterpret_cast<const char*>(static_cast<std::uintptr_t>(record.Name.Offset));
                return sink.OnSetStorageBlockBinding(record, name);
            }
            default:
                // A slot InstallMonolithVerbPort routes here whose row this switch does not know:
                // the two lists drifted, and falling through to the driver would be the very
                // "ran the other arm and looked fine" this port exists to end.
                UnmigratedVerbFatal("MonolithVerbPort");
            }
        }

        // WHERE A PORTED EMITTER'S RECORD GOES: the session's ring, or - on monolith's record arm -
        // the in-process sink above. Spelled like the session (EmitAndWait / EmitAndWaitTails /
        // MaxReplyBytes / ...) so an emitter reads the same either way.
        class VerbChannel {
        public:
            explicit VerbChannel(VerbSession* session) : m_session(session) {}

            Uint64 EmitAndWait(MG_Pipe::MGPWireOp op, const void* payload, Uint64 payloadBytes,
                               const void* varTail, Uint64 varTailBytes, void* replyOut, Uint64 replyBytes,
                               Int32* statusOut, Uint64* replySizeOut = nullptr) {
                if (m_session != nullptr) {
                    return m_session->EmitAndWait(op, payload, payloadBytes, varTail, varTailBytes, replyOut,
                                                  replyBytes, statusOut, replySizeOut);
                }
                const MG_Pipe::MGPipeVerbTail tail{varTail, varTailBytes};
                Port(op, payload, &tail, varTail != nullptr ? 1u : 0u, replyOut, replyBytes, statusOut, replySizeOut);
                return 0;
            }
            Uint64 EmitAndWaitTails(MG_Pipe::MGPWireOp op, const void* payload, Uint64 payloadBytes,
                                    const MG_Pipe::MGPipeVerbTail* tails, Uint32 tailCount, void* replyOut, Uint64 replyBytes,
                                    Int32* statusOut, Uint64* replySizeOut = nullptr) {
                if (m_session != nullptr) {
                    return m_session->EmitAndWaitTails(op, payload, payloadBytes, tails, tailCount, replyOut,
                                                       replyBytes, statusOut, replySizeOut);
                }
                Port(op, payload, tails, tailCount, replyOut, replyBytes, statusOut, replySizeOut);
                return 0;
            }
            Uint32 MaxReplyBytes() const {
                return m_session != nullptr ? m_session->MaxReplyBytes() : kMonolithMaxReplyBytes;
            }
            void RequireReadPixelsReplyFits(Uint32 width, Uint32 height, Uint32 format, Uint32 type,
                                            Uint64 bytes) const {
                if (m_session != nullptr) m_session->RequireReadPixelsReplyFits(width, height, format, type, bytes);
            }
            // A blob the record names by reference: staged in SEG_STAGE under a session, its own
            // address on the port (kMGHostSpanSegNone, the monolith spelling MGPipeHostBytes reads).
            MG_Pipe::MGPBlobRef StageBytes(const void* bytes, Uint64 size) {
                if (m_session != nullptr) return m_session->StageBytes(bytes, size);
                return MG_Pipe::MGPBlobRef{static_cast<Uint64>(reinterpret_cast<std::uintptr_t>(bytes)), size,
                                           MG_Pipe::kMGHostSpanSegNone, 0};
            }

        private:
            void Port(MG_Pipe::MGPWireOp op, const void* payload, const MG_Pipe::MGPipeVerbTail* tails, Uint32 tailCount,
                      void* replyOut, Uint64 replyBytes, Int32* statusOut, Uint64* replySizeOut) {
                MonolithReplySink replies(replyOut, replyBytes);
                const Bool applied = ApplyOnMonolithPort(op, payload, tails, tailCount, &replies);
                // A sink that declined without answering is the decoder's DECLINED; an answer it
                // posted is the answer, whatever the return.
                const Int32 status = replies.Posted()
                                         ? replies.Status()
                                         : (applied ? MG_Pipe::MGPipeReplySink::kStatusOk : MG_Pipe::MGPipeReplySink::kStatusDeclined);
                if (statusOut != nullptr) *statusOut = status;
                if (replySizeOut != nullptr) *replySizeOut = replies.Posted() ? replies.Size() : 0;
                if (!applied && replyOut == nullptr) {
                    MGLOG_E_ONCE("MGPipe: the monolith verb port's op %u was declined by the sink; nothing was "
                                 "drawn, dispatched or changed",
                                 static_cast<unsigned>(op));
                }
            }

            VerbSession* m_session;
        };

        // The channel a ported emitter uses: the port when monolith's record arm installed it
        // (only the ported slots reach an emitter there), the active session otherwise.
        VerbChannel ChannelFor(const char* slot) {
            if (g_monolithVerbPort) return VerbChannel(nullptr);
            return VerbChannel(&RequireVerbSession(slot));
        }

        // =============================================================================
        // CLASS B - the five slots the verb census measured (CONTRACT-P5.md §7)
        // =============================================================================

        void EmitClear(GLbitfield mask) {
            VerbChannel session = ChannelFor("Clear");
            BeforeReadOnlyVerb();

            if (g_dropClearEmission) {
                // A negative control. Everything above still ran, so the only difference
                // between this arm and the live one is the record - which is exactly the
                // statement "the picture comes from the wire" that E2 exists to prove. Its
                // OBSERVABILITY is a property of the workload, not of this branch: see
                // ArmControlKnobs for the measurement that took E2's retrace off this knob.
                ++g_droppedClearEmissions;
                return;
            }

            MG_Pipe::MGPClear record{};
            // The DRAW framebuffer is whatever the server's own SyncRenderState resolves from
            // gPipeInputs, which the client's MGP_FILL(Clear) at GL_Drawing.cpp:534 has just
            // written and the verb barrier keeps still (R-1). Naming a handle here would be a
            // SECOND statement of the binding, and the second one is the one that goes stale.
            record.Fbo = MG_Pipe::kMGPipeNullHandle;
            record.Kind = MG_Pipe::kMGPipeClearKindWhole;
            record.DrawBufferIndex = -1;
            record.BufferMask = static_cast<Uint32>(mask);
            record.ValueClass = 0;
            session.EmitAndWait(MG_Pipe::MGPWireOp::Clear, &record, sizeof(record), nullptr, 0,
                                nullptr, 0, nullptr);
        }


        // ---- f1: verbatim clear/copy/mipmap records (CONTRACT-P5B §2) ----
        void EmitF1Clear(const char* slot, MG_Pipe::MGPipeHandle fbo, GLenum buffer,
                         GLint drawbuffer, Uint8 valueClass, const void* value,
                         GLfloat depth = 0, GLint stencil = 0) {
            VerbChannel session = ChannelFor(slot);
            BeforeReadOnlyVerb();
            MG_Pipe::MGPClear record{};
            record.Fbo = fbo;
            record.DrawBufferIndex = drawbuffer;
            record.ValueClass = valueClass;
            switch (buffer) {
            case GL_COLOR:
                record.Kind = MG_Pipe::kMGPipeClearKindColor;
                std::memcpy(record.ColorValue, value, sizeof(record.ColorValue));
                break;
            case GL_DEPTH:
                record.Kind = MG_Pipe::kMGPipeClearKindDepth;
                std::memcpy(&record.DepthValue, value, sizeof(record.DepthValue));
                break;
            case GL_STENCIL:
                record.Kind = MG_Pipe::kMGPipeClearKindStencil;
                std::memcpy(&record.StencilValue, value, sizeof(record.StencilValue));
                break;
            case GL_DEPTH_STENCIL:
                record.Kind = MG_Pipe::kMGPipeClearKindDepthStencil;
                record.DepthValue = depth;
                record.StencilValue = stencil;
                break;
            default: UnmigratedVerbFatal(slot);
            }
            session.EmitAndWait(MG_Pipe::MGPWireOp::Clear, &record, sizeof(record),
                                nullptr, 0, nullptr, 0, nullptr);
        }

        void EmitClearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat* value) {
            EmitF1Clear("ClearBufferfv", MG_Pipe::kMGPipeNullHandle, buffer, drawbuffer,
                        MG_Pipe::kMGPipeClearValueClassFloat, value);
        }
        void EmitClearNamedFramebufferfv(const SharedPtr<MG_State::GLState::FramebufferObject>& fbo,
                                               GLenum buffer, GLint drawbuffer, const GLfloat* value) {
            EmitF1Clear("ClearNamedFramebufferfv", MG_Pipe::MGPipeFramebufferEmitter::HandleFor(*fbo),
                        buffer, drawbuffer, MG_Pipe::kMGPipeClearValueClassFloat, value);
        }

        void EmitClearBufferiv(GLenum buffer, GLint drawbuffer, const GLint* value) {
            EmitF1Clear("ClearBufferiv", MG_Pipe::kMGPipeNullHandle, buffer, drawbuffer,
                        MG_Pipe::kMGPipeClearValueClassInt, value);
        }
        void EmitClearNamedFramebufferiv(const SharedPtr<MG_State::GLState::FramebufferObject>& fbo,
                                               GLenum buffer, GLint drawbuffer, const GLint* value) {
            EmitF1Clear("ClearNamedFramebufferiv", MG_Pipe::MGPipeFramebufferEmitter::HandleFor(*fbo),
                        buffer, drawbuffer, MG_Pipe::kMGPipeClearValueClassInt, value);
        }

        void EmitClearBufferuiv(GLenum buffer, GLint drawbuffer, const GLuint* value) {
            EmitF1Clear("ClearBufferuiv", MG_Pipe::kMGPipeNullHandle, buffer, drawbuffer,
                        MG_Pipe::kMGPipeClearValueClassUint, value);
        }
        void EmitClearNamedFramebufferuiv(const SharedPtr<MG_State::GLState::FramebufferObject>& fbo,
                                               GLenum buffer, GLint drawbuffer, const GLuint* value) {
            EmitF1Clear("ClearNamedFramebufferuiv", MG_Pipe::MGPipeFramebufferEmitter::HandleFor(*fbo),
                        buffer, drawbuffer, MG_Pipe::kMGPipeClearValueClassUint, value);
        }

        void EmitClearBufferfi(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil) {
            EmitF1Clear("ClearBufferfi", MG_Pipe::kMGPipeNullHandle, buffer, drawbuffer,
                        MG_Pipe::kMGPipeClearValueClassFloat, nullptr, depth, stencil);
        }
        void EmitClearNamedFramebufferfi(const SharedPtr<MG_State::GLState::FramebufferObject>& fbo,
                                         GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil) {
            EmitF1Clear("ClearNamedFramebufferfi", MG_Pipe::MGPipeFramebufferEmitter::HandleFor(*fbo),
                        buffer, drawbuffer, MG_Pipe::kMGPipeClearValueClassFloat, nullptr, depth, stencil);
        }
        const SharedPtr<MG_State::GLState::ITextureObject>& F1BoundTexture(GLenum target) {
            auto& ctx = *MG_State::pGLContext;
            return ctx.GetTextureUnitObject(ctx.GetActiveTextureUnit())
                .GetBindingSlot(MG_Util::ConvertGLEnumToTextureTarget(target)).GetBoundObject();
        }
        void EmitF1Copy(GLenum target, GLint level, GLenum format, GLint x, GLint y,
                        GLsizei width, GLsizei height, GLint xoffset, GLint yoffset, Bool subImage) {
            VerbChannel session = ChannelFor(subImage ? "CopyTexSubImage2D" : "CopyTexImage2D");
            BeforeReadOnlyVerb();
            MG_Pipe::MGPCopyFromFramebuffer record{};
            record.Dst = MG_Pipe::MGPipeTextureEmitterInstance().FindTexture(*F1BoundTexture(target));
            record.Target = static_cast<Uint32>(target);
            record.Level = level;
            record.InternalFormat = format;
            record.X = x; record.Y = y;
            record.Width = width; record.Height = height;
            record.XOffset = xoffset; record.YOffset = yoffset;
            record.SubImage = subImage;
            session.EmitAndWait(MG_Pipe::MGPWireOp::CopyFramebufferToTexture, &record, sizeof(record),
                                nullptr, 0, nullptr, 0, nullptr);
        }
        void EmitCopyTexImage2D(GLenum target, GLint level, GLenum format, GLint x, GLint y,
                                GLsizei width, GLsizei height, GLint) {
            EmitF1Copy(target, level, format, x, y, width, height, 0, 0, false);
        }
        void EmitCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                   GLint x, GLint y, GLsizei width, GLsizei height) {
            EmitF1Copy(target, level, 0, x, y, width, height, xoffset, yoffset, true);
        }
        void EmitGenerateMipmap(GLenum target) {
            VerbChannel session = ChannelFor("GenerateMipmap");
            BeforeReadOnlyVerb();
            const auto& texture = F1BoundTexture(target);
            MG_Pipe::MGPMipPlan record{};
            record.Res = MG_Pipe::MGPipeTextureEmitterInstance().FindTexture(*texture);
            record.Target = static_cast<Uint16>(target);
            record.BaseLevel = texture->GetLevelRange().x();
            const auto* mipmap = dynamic_cast<const MG_State::GLState::TextureObjectMipmap*>(texture.get());
            if (mipmap && !texture->GetUploadTargets().empty()) {
                const auto plan = MG_Impl::GLImpl::ComputeMipmapGenerationRange(*mipmap, texture->GetUploadTargets()[0]);
                // LevelCount is the logical end-exclusive, not the number of
                // levels following BaseLevel. Preserve that existing carrier.
                record.LevelCount = static_cast<Uint16>(std::min<Uint>(plan.End, mipmap->GetMipmapLevelCount()));
            }
            session.EmitAndWait(MG_Pipe::MGPWireOp::GenerateMipmap, &record, sizeof(record),
                                nullptr, 0, nullptr, 0, nullptr);
        }

        // All draw entry points share the owned-input preparation and emission below.
        [[noreturn]] void RefuseDrawByName(const char* slot, const char* qualifier);
        void EmitDrawRecord(const char* slot, MG_Pipe::MGPDrawInfo& info,
                            const MG_Pipe::MGPDrawRange* ranges, Uint32 numDraws,
                            const void* clientIndices, Uint64 clientIndexBytes,
                            const MG_Pipe::MGPDrawIndirect* indirect);

        // Does the bound VAO fetch any ENABLED attribute out of the application's own memory.
        // The test is EmitVertexBuffers' own (`attrib.Enabled && !attrib.Buffer` is exactly what
        // it publishes as Res == kMGPipeNullHandle), so the flag on the wire and the record the
        // server applies agree by construction.
        Bool BoundVaoHasClientVertexArrays(MG_State::GLState::GLContext* ctx) {
            if (ctx == nullptr) return false;
            const auto& vao = ctx->GetBoundVertexArray();
            if (!vao) return false;
            const auto& attributes = vao->GetAllAttributes();
            for (SizeT i = 0; i < attributes.size(); ++i) {
                if (attributes[i].Enabled && !attributes[i].Buffer) return true;
            }
            return false;
        }

        void EmitDrawArrays(GLenum mode, GLint first, GLsizei count) {
            const Bool clientArrays = BoundVaoHasClientVertexArrays(MG_State::pGLContext.get());

            MG_Pipe::MGPDrawInfo info{};
            info.Mode = static_cast<Uint32>(mode);
            info.IndexSize = 0; // arrays
            // NO kDrawHasUserIndices: the reduced path draws from a VBO. kDrawClientArrays IS
            // set when one is present, because the wait rule is computed from the record on
            // both sides and this path publishes its own record rather than PlanDrawInfo's.
            info.Flags = clientArrays ? static_cast<Uint8>(MG_Pipe::kDrawClientArrays) : 0;
            info.InstanceCount = 1;
            info.StartInstance = 0;
            info.RestartIndex = 0;
            info.DrawIdOffset = 0;
            info.IndexResource = MG_Pipe::kMGPipeNullHandle;
            info.MinIndex = ~0u; // "unknown", MGPipeTypes.h:1330
            info.MaxIndex = ~0u;
            info.XfbCpuCapturedVertices = 0;
            info.NumDraws = 1;

            const MG_Pipe::MGPDrawRange range{static_cast<Uint32>(first), static_cast<Uint32>(count), 0};
            // One tail of exactly NumDraws entries. w1's encoder recomputes that from the
            // payload and Fatals on a disagreement, on THIS side - so a NumDraws that drifted
            // from the tail is a producer-side abort rather than a corrupt stream a peer has to
            // diagnose.
            EmitDrawRecord("DrawArrays", info, &range, 1, nullptr, 0, nullptr);
        }

        // =============================================================================
        // P5b d1 - the nineteen indexed / instanced / multi-draw / indirect draw slots
        // (MG_Remote/CONTRACT-P5B.md §2 d1). ONE ROW, draw_vbo (59): the record carries the GL
        // call verbatim (rule D) beside the handle the P8 form will dispatch on, and the sink
        // reproduces the backend call the monolith makes.
        // =============================================================================
        //
        // Every entry point below is: read the bindings, plan the head and the ranges (the pure
        // functions the unit cases drive), then EmitDrawRecord - which runs the SAME pre-verb
        // hooks in the SAME order as EmitDrawArrays (push, then mark walk, then the record;
        // ID-18), honours the E2 draw-drop control for every draw record and not only the P5
        // one, stages a client index array into SEG_STAGE when there is one, and emits.
        //
        // Client arrays and client indices become owned ordinary buffer resources.
        // Remaining invalid/unrepresentable draw shapes are named before encoding:
        //   +CLIENT_COMMANDS   an indirect draw with no GL_DRAW_INDIRECT_BUFFER bound: `indirect`
        //                      would be a host pointer, which rule B forbids on the wire.
        //   +UNBOUND_PARAMETER an *IndirectCount with no GL_PARAMETER_BUFFER (the frontend has
        //                      already raised INVALID_OPERATION for it; stated so the emitter
        //                      cannot send a null handle where the sink dereferences one).
        //   +INDEX_OFFSET      an element-buffer byte offset that is not a whole number of
        //                      indices (or past 2^32 of them): the record spells Start in
        //                      indices, and rounding would draw from the wrong element.

        [[noreturn]] void RefuseDrawByName(const char* slot, const char* qualifier) {
            char name[96];
            std::snprintf(name, sizeof(name), "%s+%s", slot, qualifier);
            UnmigratedVerbFatal(name);
        }

        // The bindings the plan is made from, read ONCE per draw from the frontend context on
        // the GL thread. The element buffer is the VAO's (the same slot EmitIndexBuffer read at
        // validate), the two indirect buffers are the context's, and the handles are LOOKED UP,
        // never minted: a push build mints in the BufferObject constructor.
        RemoteDrawBindings ReadDrawBindings() {
            RemoteDrawBindings b{};
            MG_State::GLState::GLContext* ctx = MG_State::pGLContext.get();
            if (ctx == nullptr) return b;
            if (const auto& vao = ctx->GetBoundVertexArray()) {
                if (const auto& bound = vao->GetIndexBufferBindingSlot().GetBoundObject()) {
                    b.ElementBufferBound = true;
                    b.ElementBuffer = MG_Pipe::MGPipeResourceTrackerInstance().Find(*bound);
                }
                // P5e (vi): the client-array probe, in the SAME single read of the bindings the
                // rest of the plan is made from rather than in a second walk at the refusal -
                // this runs on the GL thread once per draw and the refusal must not add its own
                // frontend pass. The test is the emitter's own: EmitVertexBuffers publishes
                // Res == kMGPipeNullHandle for exactly `attrib.Enabled && !attrib.Buffer`, so
                // the flag and the record agree by construction instead of by inspection.
                const auto& attributes = vao->GetAllAttributes();
                for (SizeT i = 0; i < attributes.size(); ++i) {
                    if (attributes[i].Enabled && !attributes[i].Buffer) {
                        b.ClientVertexArrays = true;
                        break;
                    }
                }
            }
            if (const auto& di = ctx->GetBufferBindingSlot(::MobileGL::BufferTarget::DrawIndirect).GetBoundObject()) {
                b.DrawIndirectBuffer = MG_Pipe::MGPipeResourceTrackerInstance().Find(*di);
            }
            if (const auto& pb = ctx->GetBufferBindingSlot(::MobileGL::BufferTarget::Parameter).GetBoundObject()) {
                b.ParameterBuffer = MG_Pipe::MGPipeResourceTrackerInstance().Find(*pb);
            }
            b.PrimitiveRestart = ctx->IsCapabilityEnabled(CapabilityInput::PrimitiveRestart) ||
                                 ctx->IsCapabilityEnabled(CapabilityInput::PrimitiveRestartFixedIndex);
            b.RestartIndex = ctx->GetPrimitiveRestartIndex();
            return b;
        }

        // The one emission for all nineteen. `clientIndices`/`clientIndexBytes` name a client
        // index array to stage (no element buffer bound); `indirect` is the kDrawIsIndirect
        // block. The two are exclusive by construction here and by the layout on both sides.
        void EmitDrawRecord(const char* slot, MG_Pipe::MGPDrawInfo& info,
                            const MG_Pipe::MGPDrawRange* ranges, Uint32 numDraws,
                            const void* clientIndices, Uint64 clientIndexBytes,
                            const MG_Pipe::MGPDrawIndirect* indirect) {
            VerbChannel session = ChannelFor(slot);
            MG_Record::PersistentMapTracker::Instance().PushDrawConsumers();
            MG_Pipe::MGPipeDrainDeferredDestroys();
            // Snapshot before marking THIS draw's potential GPU writes: resolving
            // a GPU-produced EBO here must not clear its pending mark for this draw.
            UniquePtr<MG_Pipe::MGPipeOwnedDrawInputs> ownedInputs;
            if ((info.Flags & MG_Pipe::kDrawClientArrays) != 0 || clientIndexBytes != 0) {
                if (MG_State::pGLContext == nullptr) RefuseDrawByName(slot, "NO_CONTEXT");
                ownedInputs = MakeUnique<MG_Pipe::MGPipeOwnedDrawInputs>(*MG_State::pGLContext);
                if (!ownedInputs->Prepare(info, ranges, numDraws, clientIndices, clientIndexBytes, indirect)) {
                    MG_State::pGLContext->RecordError(ErrorCode::InvalidOperation,
                        MakeUnique<GenericErrorInfo>("MG_Remote/Client", slot,
                            "the client vertex/index fetch range cannot be represented by its storage"));
                    return;
                }
            }
            MarkGpuWritesForDraw();

            if (g_dropDrawEmission) {
                // E2's negative control covers EVERY draw record, not only DrawArrays: the
                // Minecraft traces are DrawElements frames, and a control that dropped only the
                // one P5 entry point would leave those lanes green with the wire disarmed.
                ++g_droppedDrawEmissions;
                return;
            }

            info.NumDraws = numDraws;
            MG_Pipe::MGPipeVerbTail tails[2] = {{ranges, static_cast<Uint64>(numDraws) * sizeof(MG_Pipe::MGPDrawRange)},
                                       {nullptr, 0}};
            Uint32 tailCount = 1;
            if (indirect != nullptr) {
                info.Flags |= MG_Pipe::kDrawIsIndirect;
                tails[1] = {indirect, sizeof(*indirect)};
                tailCount = 2;
            }
            session.EmitAndWaitTails(MG_Pipe::MGPWireOp::DrawVbo, &info, sizeof(info), tails,
                                     tailCount, nullptr, 0, nullptr);
        }

        // ---- the single-draw indexed family: DrawElements, DrawElementsBaseVertex, the two
        // DrawRangeElements*, the four DrawElementsInstanced* -------------------------------
        void EmitIndexedDraw(const char* slot, GLenum mode, GLsizei count, GLenum type,
                             const void* indices, GLint baseVertex, GLsizei instanceCount,
                             GLuint baseInstance, Bool hasRange, GLuint start, GLuint end) {
            const RemoteDrawBindings bindings = ReadDrawBindings();
            const Uint8 indexSize = RemoteIndexSizeFor(type);
            if (indexSize == 0) RefuseDrawByName(slot, "INDEX_TYPE");
            MG_Pipe::MGPDrawInfo info =
                PlanDrawInfo(mode, indexSize, instanceCount, baseInstance, 1, bindings);
            if (hasRange) {
                info.Flags |= MG_Pipe::kDrawHasIndexRange;
                info.MinIndex = start;
                info.MaxIndex = end;
            }
            MG_Pipe::MGPDrawRange range{};
            if (!PlanDrawRange(bindings, indexSize, indices, count, baseVertex, range)) {
                RefuseDrawByName(slot, "INDEX_OFFSET");
            }
            // No element buffer: `indices` is the application's array and the draw's own
            // range is exactly what the driver would read. A null pointer or an empty draw
            // stages nothing and crosses as a zero-length range, which is what the monolith
            // hands the driver too.
            const Bool clientArray = !bindings.ElementBufferBound && indices != nullptr && count > 0;
            EmitDrawRecord(slot, info, &range, 1, clientArray ? indices : nullptr,
                           clientArray ? static_cast<Uint64>(count) * indexSize : 0, nullptr);
        }

        void EmitDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
            EmitIndexedDraw("DrawElements", mode, count, type, indices, 0, 1, 0, false, 0, 0);
        }
        void EmitDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                        GLint basevertex) {
            EmitIndexedDraw("DrawElementsBaseVertex", mode, count, type, indices, basevertex, 1, 0,
                            false, 0, 0);
        }
        void EmitDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                   const void* indices) {
            EmitIndexedDraw("DrawRangeElements", mode, count, type, indices, 0, 1, 0, true, start, end);
        }
        void EmitDrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count,
                                             GLenum type, const void* indices, GLint basevertex) {
            EmitIndexedDraw("DrawRangeElementsBaseVertex", mode, count, type, indices, basevertex, 1,
                            0, true, start, end);
        }
        void EmitDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                       GLsizei instancecount) {
            EmitIndexedDraw("DrawElementsInstanced", mode, count, type, indices, 0, instancecount, 0,
                            false, 0, 0);
        }
        void EmitDrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type,
                                                 const void* indices, GLsizei instancecount,
                                                 GLint basevertex) {
            EmitIndexedDraw("DrawElementsInstancedBaseVertex", mode, count, type, indices, basevertex,
                            instancecount, 0, false, 0, 0);
        }
        void EmitDrawElementsInstancedBaseInstance(GLenum mode, GLsizei count, GLenum type,
                                                   const void* indices, GLsizei instancecount,
                                                   GLuint baseinstance) {
            EmitIndexedDraw("DrawElementsInstancedBaseInstance", mode, count, type, indices, 0,
                            instancecount, baseinstance, false, 0, 0);
        }
        void EmitDrawElementsInstancedBaseVertexBaseInstance(GLenum mode, GLsizei count, GLenum type,
                                                             const void* indices, GLsizei instancecount,
                                                             GLint basevertex, GLuint baseinstance) {
            EmitIndexedDraw("DrawElementsInstancedBaseVertexBaseInstance", mode, count, type, indices,
                            basevertex, instancecount, baseinstance, false, 0, 0);
        }

        // ---- the instanced array draws ------------------------------------------------
        void EmitArraysDraw(const char* slot, GLenum mode, GLint first, GLsizei count,
                            GLsizei instanceCount, GLuint baseInstance) {
            const RemoteDrawBindings bindings = ReadDrawBindings();
            MG_Pipe::MGPDrawInfo info = PlanDrawInfo(mode, 0, instanceCount, baseInstance, 1, bindings);
            MG_Pipe::MGPDrawRange range{};
            PlanDrawRange(bindings, 0, reinterpret_cast<const void*>(static_cast<std::intptr_t>(first)),
                          count, 0, range);
            EmitDrawRecord(slot, info, &range, 1, nullptr, 0, nullptr);
        }
        void EmitDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instancecount) {
            EmitArraysDraw("DrawArraysInstanced", mode, first, count, instancecount, 0);
        }
        void EmitDrawArraysInstancedBaseInstance(GLenum mode, GLint first, GLsizei count,
                                                 GLsizei instancecount, GLuint baseinstance) {
            // The vertex-FETCH base instance already crossed in set_vertex_buffers::BaseInstance
            // at validate (D-H1: MGP_SET_BASE_INSTANCE runs before MGP_FILL); StartInstance is
            // gl_BaseInstance's value and feeds the GL call.
            EmitArraysDraw("DrawArraysInstancedBaseInstance", mode, first, count, instancecount,
                           baseinstance);
        }

        // ---- the multi-draws: MGPDrawRange[drawcount] is exactly their shape -------------
        //
        // The range array is built in a scratch vector owned by the GL thread (the emitter is
        // single-threaded by the ring's own SPSC contract), sized by drawcount, never held past
        // the emission. A drawcount of 0 is a call that draws nothing and emits nothing: the
        // frontend has already refused a negative one.
        Vector<MG_Pipe::MGPDrawRange>& MultiDrawScratch(Uint32 count) {
            static Vector<MG_Pipe::MGPDrawRange>& scratch = *new Vector<MG_Pipe::MGPDrawRange>();
            scratch.resize(count);
            return scratch;
        }

        void EmitMultiDrawArrays(GLenum mode, const GLint* first, const GLsizei* count, GLsizei drawcount) {
            if (drawcount <= 0 || first == nullptr || count == nullptr) return;
            const RemoteDrawBindings bindings = ReadDrawBindings();
            const auto n = static_cast<Uint32>(drawcount);
            MG_Pipe::MGPDrawInfo info = PlanDrawInfo(mode, 0, 1, 0, n, bindings);
            Vector<MG_Pipe::MGPDrawRange>& ranges = MultiDrawScratch(n);
            for (Uint32 i = 0; i < n; ++i) {
                PlanDrawRange(bindings, 0,
                              reinterpret_cast<const void*>(static_cast<std::intptr_t>(first[i])),
                              count[i], 0, ranges[i]);
            }
            EmitDrawRecord("MultiDrawArrays", info, ranges.data(), n, nullptr, 0, nullptr);
        }

        void EmitMultiIndexedDraw(const char* slot, GLenum mode, const GLsizei* count, GLenum type,
                                  const GLvoid* const* indices, GLsizei drawcount,
                                  const GLint* basevertex) {
            if (drawcount <= 0 || count == nullptr || indices == nullptr) return;
            const RemoteDrawBindings bindings = ReadDrawBindings();
            const Uint8 indexSize = RemoteIndexSizeFor(type);
            if (indexSize == 0) RefuseDrawByName(slot, "INDEX_TYPE");
            const auto n = static_cast<Uint32>(drawcount);
            MG_Pipe::MGPDrawInfo info = PlanDrawInfo(mode, indexSize, 1, 0, n, bindings);
            Vector<MG_Pipe::MGPDrawRange>& ranges = MultiDrawScratch(n);
            Vector<Uint8> ownedIndices;
            for (Uint32 i = 0; i < n; ++i) {
                if (!PlanDrawRange(bindings, indexSize, indices[i], count[i],
                                   basevertex != nullptr ? basevertex[i] : 0, ranges[i])) {
                    RefuseDrawByName(slot, "INDEX_OFFSET");
                }
                if (!bindings.ElementBufferBound) {
                    const Uint64 byteCount = static_cast<Uint64>(ranges[i].Count) * indexSize;
                    if (byteCount > std::numeric_limits<Uint32>::max() - ownedIndices.size() ||
                        (byteCount != 0 && indices[i] == nullptr)) {
                        MG_State::pGLContext->RecordError(ErrorCode::InvalidOperation,
                            MakeUnique<GenericErrorInfo>("MG_Remote/Client", slot, "invalid client index span"));
                        return;
                    }
                    ranges[i].Start = static_cast<Uint32>(ownedIndices.size() / indexSize);
                    const SizeT at = ownedIndices.size();
                    ownedIndices.resize(at + static_cast<SizeT>(byteCount));
                    if (byteCount != 0) std::memcpy(ownedIndices.data() + at, indices[i], byteCount);
                }
            }
            EmitDrawRecord(slot, info, ranges.data(), n, ownedIndices.empty() ? nullptr : ownedIndices.data(),
                           ownedIndices.size(), nullptr);
        }
        void EmitMultiDrawElements(GLenum mode, const GLsizei* count, GLenum type,
                                   const GLvoid* const* indices, GLsizei drawcount) {
            EmitMultiIndexedDraw("MultiDrawElements", mode, count, type, indices, drawcount, nullptr);
        }
        void EmitMultiDrawElementsBaseVertex(GLenum mode, const GLsizei* count, GLenum type,
                                             const GLvoid* const* indices, GLsizei drawcount,
                                             const GLint* basevertex) {
            EmitMultiIndexedDraw("MultiDrawElementsBaseVertex", mode, count, type, indices, drawcount,
                                 basevertex);
        }

        // ---- the indirect family: the block is the second tail, NumDraws is 0 -------------
        void EmitIndirectDraw(const char* slot, GLenum mode, GLenum type, const void* indirect,
                              GLsizei drawcount, GLsizei stride, GLintptr parameterOffset,
                              Bool hasParameterBuffer) {
            const RemoteDrawBindings bindings = ReadDrawBindings();
            const Uint8 indexSize = type != 0 ? RemoteIndexSizeFor(type) : 0;
            if (type != 0 && indexSize == 0) RefuseDrawByName(slot, "INDEX_TYPE");
            if (MG_Pipe::MGPipeHandleIsNull(bindings.DrawIndirectBuffer)) {
                RefuseDrawByName(slot, "CLIENT_COMMANDS");
            }
            if (hasParameterBuffer && MG_Pipe::MGPipeHandleIsNull(bindings.ParameterBuffer)) {
                RefuseDrawByName(slot, "UNBOUND_PARAMETER");
            }
            MG_Pipe::MGPDrawInfo info = PlanDrawInfo(mode, indexSize, 1, 0, 0, bindings);
            const MG_Pipe::MGPDrawIndirect block = PlanDrawIndirect(bindings, indirect, drawcount, stride,
                                                                    parameterOffset, hasParameterBuffer);
            EmitDrawRecord(slot, info, nullptr, 0, nullptr, 0, &block);
        }
        void EmitDrawArraysIndirect(GLenum mode, const void* indirect) {
            EmitIndirectDraw("DrawArraysIndirect", mode, 0, indirect, 1, 0, 0, false);
        }
        void EmitDrawElementsIndirect(GLenum mode, GLenum type, const void* indirect) {
            EmitIndirectDraw("DrawElementsIndirect", mode, type, indirect, 1, 0, 0, false);
        }
        void EmitMultiDrawArraysIndirect(GLenum mode, const void* indirect, GLsizei drawcount,
                                         GLsizei stride) {
            EmitIndirectDraw("MultiDrawArraysIndirect", mode, 0, indirect, drawcount, stride, 0, false);
        }
        void EmitMultiDrawElementsIndirect(GLenum mode, GLenum type, const void* indirect,
                                           GLsizei drawcount, GLsizei stride) {
            EmitIndirectDraw("MultiDrawElementsIndirect", mode, type, indirect, drawcount, stride, 0,
                             false);
        }
        void EmitMultiDrawArraysIndirectCount(GLenum mode, const void* indirect, GLintptr drawcount,
                                              GLsizei maxdrawcount, GLsizei stride) {
            EmitIndirectDraw("MultiDrawArraysIndirectCount", mode, 0, indirect, maxdrawcount, stride,
                             drawcount, true);
        }
        void EmitMultiDrawElementsIndirectCount(GLenum mode, GLenum type, const void* indirect,
                                                GLintptr drawcount, GLsizei maxdrawcount,
                                                GLsizei stride) {
            EmitIndirectDraw("MultiDrawElementsIndirectCount", mode, type, indirect, maxdrawcount,
                             stride, drawcount, true);
        }

        void EmitBlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0,
                                 GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask,
                                 GLenum filter) {
            VerbChannel session = ChannelFor("BlitFramebuffer");
            BeforeReadOnlyVerb();

            MG_Pipe::MGPBlit record{};
            // Same reasoning as Clear's Fbo: the read and draw bindings are gPipeInputs', set
            // by MGP_FILL(BlitFramebuffer) at GL_Framebuffer.cpp:660 and held still by the
            // barrier. The named form below carries both handles after a scoped binding override.
            record.ReadFbo = MG_Pipe::kMGPipeNullHandle;
            record.DrawFbo = MG_Pipe::kMGPipeNullHandle;
            record.SrcX0 = srcX0;
            record.SrcY0 = srcY0;
            record.SrcX1 = srcX1;
            record.SrcY1 = srcY1;
            record.DstX0 = dstX0;
            record.DstY0 = dstY0;
            record.DstX1 = dstX1;
            record.DstY1 = dstY1;
            record.Mask = static_cast<Uint32>(mask);
            record.Filter = static_cast<Uint32>(filter);
            session.EmitAndWait(MG_Pipe::MGPWireOp::Blit, &record, sizeof(record), nullptr, 0,
                                nullptr, 0, nullptr);
        }

        void EmitBlitNamedFramebuffer(
            const SharedPtr<MG_State::GLState::FramebufferObject>& read,
            const SharedPtr<MG_State::GLState::FramebufferObject>& draw,
            GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0,
            GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter) {
            VerbChannel session = ChannelFor("BlitNamedFramebuffer");
            // P5c (hd, CONTRACT-P5C §3.3): the scoped rebind of the client's own read/draw
            // binding slots is DELETED. It existed only to stage values for the server's
            // binding-slot read, and that read is gone: the sink resolves both framebuffers
            // from the record's handles. The validate still refreshes both framebuffers'
            // emitted state (their Named set_framebuffer_state records), which is now the
            // only description the server syncs from. No driver call or binding change
            // happens on the client.
            MG_Pipe::MGPipeValidateForVerb(MG_Pipe::MGPipeVerb::BlitNamedFramebuffer);
            BeforeReadOnlyVerb();
            MG_Pipe::MGPBlit record{};
            record.ReadFbo = MG_Pipe::MGPipeFramebufferEmitter::HandleFor(*read);
            record.DrawFbo = MG_Pipe::MGPipeFramebufferEmitter::HandleFor(*draw);
            record.SrcX0 = srcX0; record.SrcY0 = srcY0;
            record.SrcX1 = srcX1; record.SrcY1 = srcY1;
            record.DstX0 = dstX0; record.DstY0 = dstY0;
            record.DstX1 = dstX1; record.DstY1 = dstY1;
            record.Mask = static_cast<Uint32>(mask);
            record.Filter = static_cast<Uint32>(filter);
            session.EmitAndWait(MG_Pipe::MGPWireOp::Blit, &record, sizeof(record), nullptr, 0,
                                nullptr, 0, nullptr);
        }

        // How many bytes glReadPixels will pack for this rectangle, from the PACK half of the
        // pixel-store state.
        //
        // ID-49: THE PACK STATE NEVER CROSSES FOR A READ, AND DstSize IS THE TIGHT EXTENT.
        // The first version of this computed GL 4.6 8.4.4's PACKED size - row length, skips,
        // alignment - and handed it over as DstSize. v1's server allocates exactly DstSize and
        // the real backend honours the live pack state, so a 4x3 RGBA8 read with
        // PACK_ROW_LENGTH=8, SKIP_ROWS=1, SKIP_PIXELS=2 allocated 80 bytes and the driver wrote
        // to byte 120. That is the shape of the joint inproc lane's two
        // DepthReadbackHonoursThePackPixelStoreParameters SEGFAULTs, on both backends.
        //
        // So the wire carries a RECTANGLE and not a layout: the server reads with NEUTRAL pack
        // state into a tight w*h*bytesPerPixel run that IS the reply payload, and the CLIENT -
        // which is the side that holds the application's pack state, and the only side that
        // can - scatters those rows into the application's pointer. "OnReadPixels writes
        // exactly DstSize bytes" still holds; DstSize is now a number both sides derive from
        // the same three values instead of one side deriving it from state the other cannot
        // see.
        //
        // IT IS FORMAT-AGNOSTIC ON PURPOSE. The depth and depth-stencil reads the 21 split
        // entries touch take the same rule with no special case, because the rule is about the
        // LAYOUT and not about the component: bytesPerPixel is whatever the format sizes to.
        Uint64 ReadbackBytesPerPixel(GLenum format, GLenum type) {
            const TextureInputFormat inputFormat =
                MG_Util::ConvertGLEnumToTextureInputFormat(format);
            const TexturePixelDataType dataType = MG_Util::ConvertGLEnumToTexturePixelDataType(type);
            const SizeT bytesPerPixel = MG_Util::GetInputBytesPerPixel(inputFormat, dataType);
            if (bytesPerPixel == 0) {
                // NOT a guess and not a zero-length reply. A format this build cannot size is a
                // readback whose answer would be silently truncated, which is the one failure a
                // picture comparison cannot see.
                MG_Pipe::MGPipeRecordFail(MG_Pipe::MGFatalFamily::UnsizedReadback, "MGPipe: Fatal{UnsizedReadback, \"read_pixels\"} format=0x%04x type=0x%04x "
                        "- the client must declare MGPReadbackInfo::DstSize and cannot size this "
                        "pair; P5's reduced path reads RGBA/UNSIGNED_BYTE",
                        static_cast<unsigned>(format), static_cast<unsigned>(type));
            }
            return static_cast<Uint64>(bytesPerPixel);
        }

        Uint64 TightReadbackBytes(GLsizei width, GLsizei height, GLenum format, GLenum type) {
            if (width <= 0 || height <= 0) return 0;
            return static_cast<Uint64>(width) * static_cast<Uint64>(height) *
                   ReadbackBytesPerPixel(format, type);
        }

        // GL_PACK_SWAP_BYTES as the width of the byte group it reverses for `type`, 0 when there
        // is nothing to reverse. One rule for both places a swap happens: the reply form swaps
        // here, and the pack-buffer form (P9) carries the number to the server, which swaps there.
        Uint32 ReadbackSwapGroup(GLenum type, const PixelStoreParameters& pack) {
            if (!pack.SwapBytes) return 0;
            const auto dataType = MG_Util::ConvertGLEnumToTexturePixelDataType(type);
            SizeT group = MG_Util::GetSizedTexturePixelDataTypeSize(dataType);
            if (group == 0) group = MG_Util::GetBaseTexturePixelDataTypeSize(dataType);
            // This packed depth/stencil type contains two independent 32-bit words.
            if (type == GL_FLOAT_32_UNSIGNED_INT_24_8_REV) group = 4;
            return group <= 1 ? 0 : static_cast<Uint32>(group);
        }

        void ApplyReadbackByteSwap(void* pixels, Uint64 bytes, GLenum type,
                                   const PixelStoreParameters& pack) {
            const SizeT group = ReadbackSwapGroup(type, pack);
            if (group == 0 || pixels == nullptr) return;
            auto* data = static_cast<Uint8*>(pixels);
            for (Uint64 at = 0; at + group <= bytes; at += group) {
                for (SizeT i = 0; i < group / 2; ++i) {
                    std::swap(data[at + i], data[at + group - 1 - i]);
                }
            }
        }

        // M2 / codex 11: the reply the server posted is COMPLETE and OK. A short OK reply, and a
        // DECLINED or ERROR reply with a zero payload, both leave the destination full of stale
        // bytes; scattering or returning it is the silently truncated picture ID-47's own comment
        // says an SSIM comparison cannot see, arriving through the status field rather than
        // through truncation. `expected` is the record's own DstSize (CONTRACT-P5 row 23: the
        // reply's exact extent). The predicate is at namespace scope (below) and exposed for the
        // control, so R-16's "drive the production predicate" holds rather than a second copy of
        // the rule in the test.

        // How long a declined readback waits for the control plane or the bell to say "lost"
        // before it is judged a refusal. The SessionFault is sent before the decline is posted,
        // so this is only ever spent on a server that refused for real.
        constexpr Uint32 kDeclineVerdictMs = 1000;

        // The same predicate at the call site, with the named Fatal each failure mode owns.
        // status > expected cannot reach here: EmitAndWait already aborts Fatal{ReplyTooLarge} on
        // an oversize reply, so the only failures left are a wrong status or a SHORT one.
        //
        // A LOST DEVICE IS NOT ONE OF THOSE FAILURES. Once the session is device-lost every verb
        // is declined by EmitAndWait itself, and the GL answer for a lost context is that its
        // commands do nothing: the read writes no pixel and the application learns of the loss
        // from glGetGraphicsResetStatus. Dying here instead turned a server-side GPU fault into a
        // crash of the client (Chrome's in-process GPU thread took the browser with it). False:
        // the caller leaves the destination untouched and returns.
        Bool RequireReadbackReplyComplete(Int32 status, Uint64 replySize, Uint64 expected) {
            if (status != MG_Pipe::MGPipeReplySink::kStatusOk && MG_Pipe::MGPipeClientDeviceLost()) return false;
            // THE DECLINE CAN OUTRUN THE NEWS. A server whose device was lost inside THIS readback
            // latches the session - publishing its SessionFault on the control plane - and then
            // declines the record; the client can read the declined answer before it has looked at
            // the control plane or seen the hangup that follows. So a decline asks once more,
            // briefly, before it is taken for a server without GL.ReadPixels.
            if (status == MG_Pipe::MGPipeReplySink::kStatusDeclined) {
                if (MG_Pipe::MGPipeClientConfirmLossAfterDecline(kDeclineVerdictMs)) return false;
            }
            if (status == MG_Pipe::MGPipeReplySink::kStatusError) {
                MG_Pipe::MGPipeRecordFail(MG_Pipe::MGFatalFamily::ReplyError, "MGPipe: Fatal{ReplyError, \"ReadPixels\"} - the readback answered ERROR; "
                        "the destination is left untouched rather than filled with stale bytes");
            }
            if (status == MG_Pipe::MGPipeReplySink::kStatusDeclined) {
                MG_Pipe::MGPipeRecordFail(MG_Pipe::MGFatalFamily::ReadbackDeclined, "MGPipe: Fatal{ReadbackDeclined, \"ReadPixels\"} - the server has no "
                        "GL.ReadPixels and DECLINED; a decline is a real answer for an acceptance "
                        "row (R-5) but a blocking readback has no pixels to return, so it is a "
                        "Fatal here rather than a buffer of stale bytes");
            }
            if (status != MG_Pipe::MGPipeReplySink::kStatusOk) {
                MG_Pipe::MGPipeRecordFail(MG_Pipe::MGFatalFamily::ReplyStatusInvalid, "MGPipe: Fatal{ReplyStatusInvalid, \"ReadPixels\"} - unknown reply status %d", status);
            }
            if (replySize != expected) {
                MG_Pipe::MGPipeRecordFail(MG_Pipe::MGFatalFamily::ReadbackReplyShort, "MGPipe: Fatal{ReadbackReplyShort, \"ReadPixels %llu < %llu\"} - the OK "
                        "reply carried fewer bytes than the read's own DstSize (CONTRACT-P5 row "
                        "23's exact extent); the missing rows would otherwise be scattered as "
                        "whatever the destination held",
                        static_cast<unsigned long long>(replySize),
                        static_cast<unsigned long long>(expected));
            }
            return true;
        }

#include "TextureReadbackEmit.inc"

        void EmitReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format,
                            GLenum type, void* pixels) {
            VerbChannel session = ChannelFor("ReadPixels");

            // A bound PACK buffer makes pixels an offset. The owned reply is
            // packed on this thread and only the requested rows are uploaded.
            const auto pbo = MG_State::pGLContext != nullptr
                ? MG_State::pGLContext->GetBufferBindingSlot(::MobileGL::BufferTarget::PixelPack).GetBoundObject()
                : SharedPtr<MG_State::GLState::BufferObject>{};

            BeforeReadOnlyVerb();

            if (width <= 0 || height <= 0) return;
            const Uint64 bytesPerPixel = ReadbackBytesPerPixel(format, type);

            PixelStoreParameters pack{};
            if (MG_State::pGLContext != nullptr) {
                pack = MG_State::pGLContext->GetPixelStoreParameters(/*isUnpack=*/false);
            }
            ReadbackLayout layout;
            const SizeT pboOffset = reinterpret_cast<SizeT>(pixels);
            if (!CheckedReadbackLayout(width, height, 1, bytesPerPixel, pack, false, layout) ||
                (pbo && (pboOffset > pbo->GetSize() || layout.End > pbo->GetSize() - pboOffset))) {
                RecordReadbackRangeError("ReadPixels");
                return;
            }

            // P7 GATE 5 (g5-readback): A READ LARGER THAN ONE REPLY SLOT IS BANDED, NOT REFUSED.
            // KHR-GL46.direct_state_access.renderbuffers_storage reads 256x512 RGBA/FLOAT =
            // 2,097,152 bytes, sixteen over MaxReplyBytes, and that used to be
            // Fatal{ReplyTooLarge} on every split arm while the monolith passed. Each band below
            // is an ordinary read_pixels record - its own box, and a DstSize that IS its tight
            // w*h*bpp extent (ID-49) - so every answer still fits one slot (ID-47) and the
            // server's PH-3 bound (its tight answer against its own LinkTerms.maxReplyBytes,
            // PipeApplier.cpp OnReadPixels) holds unchanged: no reply is chunked, the READ is. The cap is the link's own, read live, exactly as
            // before, so a different SEG_REPLY geometry or a stream link needs no edit here.
            ReadbackBandPlan plan;
            if (!PlanReadbackBands(static_cast<Uint64>(width), static_cast<Uint64>(height),
                                   bytesPerPixel, session.MaxReplyBytes(), plan)) {
                // NOT EVEN ONE PIXEL FITS (or there is no reply pool at all). That is the only
                // read no banding can answer, and ID-47's named refusal is kept for it - for the
                // one-pixel piece that cannot be sent, so the message names what was impossible.
                session.RequireReadPixelsReplyFits(1, 1, static_cast<Uint32>(format),
                                                   static_cast<Uint32>(type), bytesPerPixel);
                return;
            }

            // One band's source record, the same for both destinations below.
            const auto bandInfo = [&](const ReadbackBand& band, Uint64 bandBytes) {
                MG_Pipe::MGPReadbackInfo info{};
                info.Res = MG_Pipe::kMGPipeNullHandle; // "the bound read surface answers"
                info.Box = MG_Pipe::MGPBox{ReadbackBandOrigin(x, band.FirstColumn),
                                           ReadbackBandOrigin(y, band.FirstRow), 0,
                                           static_cast<Uint32>(band.Columns),
                                           static_cast<Uint32>(band.Rows), 1};
                info.Format = static_cast<Uint32>(format);
                info.Type = static_cast<Uint32>(type);
                info.Target = 0;
                info.Level = 0;
                info.DstOffset = 0;
                info.DstSize = bandBytes;
                return info;
            };

            // P9 (W1, CONTRACT-P9.md §1): A PACK BUFFER IS ANSWERED BY NOTHING. Each band crosses
            // as read_pixels_to_buffer naming the buffer and the band's place in it - the
            // layout above, per band - and the client neither waits nor copies: the server lands
            // the rows, and the mark (taken first, like every producer row's) is what sends the
            // next CPU read of the buffer through resource_readback. The bands are the reply
            // form's for the SERVER's sake, not the wire's: the server reads each one into its
            // scratch before landing it, and the reply cap is the bound it already enforces on
            // that scratch (PH-3). The monolith arm is untouched - it maps the driver PBO back
            // inside its own ReadPixels, as it always has.
            if (pbo) {
                const MG_Pipe::MGPipeHandle target = PackBufferReadbackTarget(pbo, pboOffset + layout.End);
                if (!MG_Pipe::MGPipeHandleIsNull(target)) {
                    MarkReadPixelsPackBuffer();
                    const Uint32 swapGroup = ReadbackSwapGroup(type, pack);
                    ForEachReadbackBand(static_cast<Uint64>(width), static_cast<Uint64>(height), plan,
                                        [&](const ReadbackBand& band) {
                        MG_Pipe::MGPReadbackToBuffer record{};
                        record.Src = bandInfo(band, TightReadbackBytes(static_cast<GLsizei>(band.Columns),
                                                                       static_cast<GLsizei>(band.Rows),
                                                                       format, type));
                        record.Dst = target;
                        record.DstBase = pboOffset + layout.Start + band.FirstRow * layout.RowStride +
                                         band.FirstColumn * bytesPerPixel;
                        record.RowStride = layout.RowStride;
                        record.ImageStride = 0;
                        record.SwapGroup = swapGroup;
                        session.EmitAndWait(MG_Pipe::MGPWireOp::ReadPixelsToBuffer, &record,
                                            sizeof(record), nullptr, 0, nullptr, 0, nullptr);
                    });
                    return;
                }
            }

            // THE COMMON CASE KEEPS THE ZERO-COPY. A neutral pack state means the destination
            // layout IS the tight layout, so each band's reply lands straight in the
            // application's buffer at the band's tight offset (whole-width bands and single-row
            // pieces are both contiguous there). It is a fast path for the SAME bytes, not a
            // second rule: ScatterReadbackBandIntoPackState is a memcpy of the band in exactly
            // this case, and the control drives that function.
            const Bool direct = !pbo && ReadbackPackStateIsTight(width, bytesPerPixel, pack);
            // The bounce is the price of the application having asked for a layout. It is ONE
            // band - at most a reply slot - rather than the whole read, and it is freed before
            // this returns: R-11's rule one level out, nothing here outlives the call.
            Vector<Uint8> bounce;
            Bool pboSynced = false;
            ForEachReadbackBand(static_cast<Uint64>(width), static_cast<Uint64>(height), plan,
                                [&](const ReadbackBand& band) {
                // ONE tight-size function for production AND the control (M3 / codex 10a): the
                // number the server allocates and the number the test asserts come from the
                // same body, per band now.
                const Uint64 bandBytes = TightReadbackBytes(static_cast<GLsizei>(band.Columns),
                                                            static_cast<GLsizei>(band.Rows),
                                                            format, type);
                MG_Pipe::MGPReadbackInfo info = bandInfo(band, bandBytes);

                // CHECKED BEFORE THE EMISSION, not after the answer (ID-47), through S1's
                // helper, for every record. The plan makes it true by construction; it stays
                // because it is the one check that names the read at the call site if the plan
                // and the pool ever disagree, where the server's PH-3 refusal would only answer
                // ERROR on the apply thread.
                session.RequireReadPixelsReplyFits(static_cast<Uint32>(band.Columns),
                                                   static_cast<Uint32>(band.Rows),
                                                   static_cast<Uint32>(format),
                                                   static_cast<Uint32>(type), bandBytes);

                Int32 status = 0;
                Uint64 replySize = 0;
                if (direct) {
                    Uint8* at = pixels == nullptr
                        ? nullptr
                        : static_cast<Uint8*>(pixels) +
                              (band.FirstRow * static_cast<Uint64>(width) + band.FirstColumn) *
                                  bytesPerPixel;
                    session.EmitAndWait(MG_Pipe::MGPWireOp::ReadPixels, &info, sizeof(info),
                                        nullptr, 0, at, bandBytes, &status, &replySize);
                    // M2 / codex 11: an OK reply that arrived short, or a DECLINE/ERROR, must not
                    // be handed back as pixels. The Fatal aborts before the application reads the
                    // buffer, so the bytes EmitAndWait already copied are never observed.
                    if (!RequireReadbackReplyComplete(status, replySize, bandBytes)) return;
                    ApplyReadbackByteSwap(at, bandBytes, type, pack);
                    return;
                }

                if (bounce.size() < bandBytes) bounce.resize(static_cast<SizeT>(bandBytes));
                session.EmitAndWait(MG_Pipe::MGPWireOp::ReadPixels, &info, sizeof(info), nullptr,
                                    0, bounce.data(), bandBytes, &status, &replySize);
                // BEFORE THE SCATTER, so a short or non-OK reply never reaches the application's
                // pointer at all (the bounce is the only thing that held the partial bytes).
                if (!RequireReadbackReplyComplete(status, replySize, bandBytes)) return;
                ApplyReadbackByteSwap(bounce.data(), bandBytes, type, pack);
                if (pbo) {
                    // pixels is an offset, never a host pointer. Upload only the requested
                    // rows so PACK padding and untouched bytes survive; the offsets are the
                    // overflow-checked layout's, the same numbers the range check accepted.
                    if (!pboSynced) {
                        pbo->SyncGpuWrites();
                        pboSynced = true;
                    }
                    const Uint64 bandRowBytes = band.Columns * bytesPerPixel;
                    for (Uint64 row = 0; row < band.Rows; ++row) {
                        pbo->UploadSubData({bounce.data() + row * bandRowBytes,
                                            static_cast<SizeT>(bandRowBytes)},
                                           pboOffset + layout.Start +
                                               (band.FirstRow + row) * layout.RowStride +
                                               band.FirstColumn * bytesPerPixel);
                    }
                    return;
                }
                ScatterReadbackBandIntoPackState(bounce.data(), pixels, width, band, bytesPerPixel,
                                                 pack);
            });
        }

        // =============================================================================
        // CLASS B - P5b package i1: image bind, compute, barriers, copy-image, SSBO block
        // (MG_Remote/CONTRACT-P5B.md §2 i1). Seven slots, five wire rows.
        // =============================================================================
        //
        // RULE D, WHICH IS WHY THESE ARE SHORT. A P5b verb crosses AS THE CALL: the record
        // carries the GL arguments verbatim - the enums as tokens, the GL names the backend
        // keys on - beside the handle the P7/P8 form will dispatch on instead, and the server's
        // ServerVerbSink reproduces the backend call the monolith makes. The backend keeps
        // reading the frontend state it reads today through the BARRIER-PULLED fields of its
        // verb class, which the record's own verb stamp is what makes legal. So a migration is
        // one emitter here plus one sink body there, and nothing in the backend moves.
        //
        // THE HANDLES ARE LOOKED UP, NEVER MINTED, and that is a ruling (i1-v1 §4). The sinks
        // below dispatch on the GL NAME - that is the whole point of carrying it - so a handle
        // is carried for P7's sake only. FindByLifetimeId answers the handle the resource
        // subsystem has already published for this object and kMGPipeNullHandle when it has
        // published none; Acquire would MINT one here instead, at a call site that emits no
        // create record, and the server would then be handed an identity it has never seen.
        // A null Res/Src/Dst/ShaderCso therefore means "no handle published yet", which is a
        // true statement, rather than a slot nobody allocated.

        MG_Pipe::MGPipeHandle PublishedTextureHandle(
            const SharedPtr<MG_State::GLState::ITextureObject>& texture) {
            if (!texture) return MG_Pipe::kMGPipeNullHandle;
            return MG_Pipe::MGPipeSlots().FindByLifetimeId(MG_Pipe::MGPipeKind::Texture,
                                                           texture->GetLifetimeId());
        }

        // glBindImageTexture. Emitted AT THE CALL, after the frontend has written the unit's
        // ImageTextureBinding and MGP_FILL(BindImageTexture) has run - the record's verb
        // boundary is what makes the server's read of that binding legal. set_shader_images
        // (the draw-prep set) still travels at the next validate, untouched: that record
        // describes a resolved unit for the draw, this one reproduces a call.
        //
        // NO PRE-VERB HOOK, AND THAT IS DELIBERATE. b1's two hooks describe "the work the
        // record is ABOUT TO START" - PushPersistentMapsBeforeVerb publishes bytes an
        // application wrote through a coherent map, MarkGpuWrites* builds the GPU-write set.
        // A bind starts no shader and reads no buffer; the dispatch that later reads this image
        // is the verb that carries both hooks, and running them here as well would push the
        // same maps twice per dispatch and inflate b1's per-row counters.
        void EmitBindImageTexture(GLuint unit, GLuint texture, GLint level, GLboolean layered,
                                  GLint layer, GLenum access, GLenum format) {
            VerbChannel session = ChannelFor("BindImageTexture");

            MG_Pipe::MGPImageBind record{};
            // The unit's binding is the frontend's and has just been written by the caller, so
            // the texture this record names is the one the server's SyncImageTextureBinding
            // will pull for the same unit. Read from MG_State::pGLContext and NOT through
            // gPipeInputs: on this side of a split gPipeInputs is gPipeInputs, which is the SERVER's
            // view, and the client asking it a question is how the two halves come to disagree.
            if (MG_State::pGLContext != nullptr) {
                record.Res = PublishedTextureHandle(
                    MG_State::pGLContext->GetImageTextureBinding(static_cast<Int>(unit)).Texture);
            }
            record.Unit = static_cast<Uint32>(unit);
            // The GL name the application passed, verbatim - what the ES slot is handed as
            // `texture` and currently ignores. Never an identity (ARCHITECTURE 4.2.1).
            record.GlName = static_cast<Uint32>(texture);
            record.Level = static_cast<Int32>(level);
            record.Layer = static_cast<Int32>(layer);
            // THE GL ACCESS TOKEN, not MGPImageView::Access's three-value encoding (table 0's
            // MGPImageBind::Access row). Two records, two jobs.
            record.Access = static_cast<Uint32>(access);
            record.Format = static_cast<Uint32>(format);
            record.Layered = layered != GL_FALSE ? 1 : 0;
            session.EmitAndWait(MG_Pipe::MGPWireOp::BindShaderImage, &record, sizeof(record),
                                nullptr, 0, nullptr, 0, nullptr);
        }

        // glDispatchCompute. THE HOOK ORDER IS b1's AND IS INHERITED FROM THE CLASS-C STUB
        // VERBATIM: push the persistent maps (they produce resource_subdata records that must
        // precede the verb on SEG_CMD), then the dispatch mark walk, then the record. The stub
        // carried both calls before its Fatal precisely so that the package which flipped this
        // slot would inherit a call site that was already correct.
        void EmitDispatchCompute(GLuint numGroupsX, GLuint numGroupsY, GLuint numGroupsZ) {
            VerbChannel session = ChannelFor("DispatchCompute");
            MG_Record::PersistentMapTracker::Instance().PushDrawConsumers();
            MarkGpuWritesForDispatch();

            MG_Pipe::MGPGridInfo record{};
            record.GridX = static_cast<Uint32>(numGroupsX);
            record.GridY = static_cast<Uint32>(numGroupsY);
            record.GridZ = static_cast<Uint32>(numGroupsZ);
            // Block* STAY 0 IN P5b (contract i1): the local size is a link artifact the backend
            // reads from its own program, and a client-minted copy would be a second statement
            // of it. P7's Magma may fill it from the reflection archive.
            record.IndirectBuffer = MG_Pipe::kMGPipeNullHandle;
            record.IndirectOffset = 0;
            record.IsIndirect = 0;
            session.EmitAndWait(MG_Pipe::MGPWireOp::LaunchGrid, &record, sizeof(record), nullptr, 0,
                                nullptr, 0, nullptr);
        }

        void EmitDispatchComputeIndirect(GLintptr indirect) {
            VerbChannel session = ChannelFor("DispatchComputeIndirect");
            MG_Record::PersistentMapTracker::Instance().PushDrawConsumers();
            MarkGpuWritesForDispatch();

            MG_Pipe::MGPGridInfo record{};
            // The counts come from the GL_DISPATCH_INDIRECT_BUFFER, so the three grid fields are
            // 0 and IsIndirect is what says so; the sink dispatches on it and calls
            // glDispatchComputeIndirect with the offset the application spelled.
            record.IsIndirect = 1;
            record.IndirectOffset = static_cast<Uint64>(indirect);
            record.IndirectBuffer = MG_Pipe::kMGPipeNullHandle;
            if (MG_State::pGLContext != nullptr) {
                const auto& bound =
                    MG_State::pGLContext
                        ->GetBufferBindingSlot(::MobileGL::BufferTarget::DispatchIndirect)
                        .GetBoundObject();
                if (bound) {
                    record.IndirectBuffer = MG_Pipe::MGPipeSlots().FindByLifetimeId(
                        MG_Pipe::MGPipeKind::Buffer, bound->GetLifetimeId());
                }
            }
            session.EmitAndWait(MG_Pipe::MGPWireOp::LaunchGrid, &record, sizeof(record), nullptr, 0,
                                nullptr, 0, nullptr);
        }

        // glMemoryBarrier / glMemoryBarrierByRegion. The bits cross VERBATIM: the frontend has
        // already validated them and already folds glTextureBarrier onto the same field
        // (GL_Drawing.cpp:920-937), and Espryt's atomic-counter lowering - the counter bit
        // implying the storage bit - stays inside the backend where the reason for it lives
        // (DirectGLES.cpp:8837). A client that pre-lowered would be answering a driver question
        // from the wrong side and the two arms would stop being byte-identical.
        //
        // No pre-verb hook: a barrier orders memory the GPU already holds. It starts no shader
        // and reads no mapped buffer.
        void EmitMemoryBarrier(GLbitfield barriers) {
            VerbChannel session = ChannelFor("MemoryBarrier");
            MG_Pipe::MGPMemoryBarrier record{};
            record.Bits = static_cast<Uint32>(barriers);
            record.ByRegion = 0;
            session.EmitAndWait(MG_Pipe::MGPWireOp::MemoryBarrier, &record, sizeof(record), nullptr,
                                0, nullptr, 0, nullptr);
        }

        void EmitMemoryBarrierByRegion(GLbitfield barriers) {
            VerbChannel session = ChannelFor("MemoryBarrierByRegion");
            MG_Pipe::MGPMemoryBarrier record{};
            record.Bits = static_cast<Uint32>(barriers);
            record.ByRegion = 1;
            session.EmitAndWait(MG_Pipe::MGPWireOp::MemoryBarrier, &record, sizeof(record), nullptr,
                                0, nullptr, 0, nullptr);
        }

        // glCopyImageSubData -> resource_copy_region (53), which P5b rules is glCopyImageSubData
        // ONLY (contract §6.4; the framebuffer-sourced copies are f1's row 76).
        void EmitCopyImageSubData(const MG_Backend::CopyImageEndpoint& src, GLenum srcTarget,
                                  GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ,
                                  const MG_Backend::CopyImageEndpoint& dst, GLenum dstTarget,
                                  GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ,
                                  GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth) {
            VerbChannel session = ChannelFor("CopyImageSubData");

            // The GL target selects the texture or renderbuffer handle namespace.
            MG_Pipe::MGPCopyRegion record{};
            const auto handle = [](const MG_Backend::CopyImageEndpoint& endpoint) {
                return endpoint.IsRenderbuffer()
                    ? MG_Pipe::MGPipeSlots().FindByLifetimeId(MG_Pipe::MGPipeKind::Renderbuffer,
                                                              endpoint.Renderbuffer->GetLifetimeId())
                    : PublishedTextureHandle(endpoint.Texture);
            };
            record.Src = handle(src);
            record.Dst = handle(dst);
            // GL names remain diagnostic only; backend identity comes from handles.
            record.SrcGlName =
                src.IsRenderbuffer() ? src.Renderbuffer->GetExternalIndex() : src.Texture->GetExternalIndex();
            record.DstGlName =
                dst.IsRenderbuffer() ? dst.Renderbuffer->GetExternalIndex() : dst.Texture->GetExternalIndex();
            // The GL targets verbatim, in a Uint16 - every GL texture target fits one. NOT
            // MGPipeResourceTarget: the sink only ever forwards these to a slot that takes GL
            // enums, and the tree has no resource-target -> GL-enum inverse to spend on them.
            record.SrcTarget = static_cast<Uint16>(srcTarget);
            record.DstTarget = static_cast<Uint16>(dstTarget);
            record.SrcLevel = static_cast<Uint16>(srcLevel);
            record.DstLevel = static_cast<Uint16>(dstLevel);
            // SrcBox is {srcX, srcY, srcZ, w, h, d}: the source origin AND the extent, which is
            // one extent for both endpoints (GL spells the copy's size once).
            record.SrcBox = MG_Pipe::MGPBox{srcX, srcY, srcZ, static_cast<Uint32>(srcWidth),
                                            static_cast<Uint32>(srcHeight),
                                            static_cast<Uint32>(srcDepth)};
            record.DstX = dstX;
            record.DstY = dstY;
            record.DstZ = dstZ;
            session.EmitAndWait(MG_Pipe::MGPWireOp::ResourceCopyRegion, &record, sizeof(record),
                                nullptr, 0, nullptr, 0, nullptr);
        }

        // glShaderStorageBlockBinding -> set_storage_block_binding (75), the ONE content-carrying
        // row P5b adds. The block is NAMED, not indexed, because the application's index is the
        // frontend interface-query enumeration's and no backend shares that index space
        // (BackendObject.h:216-221) - the name is the one coordinate all three agree on.
        void EmitShaderStorageBlockBinding(GLuint program, const GLchar* storageBlockName,
                                           GLuint storageBlockBinding) {
            VerbChannel session = ChannelFor("ShaderStorageBlockBinding");
            // The backend slot's own first line (DirectGLES.cpp:9201), kept here so a null name
            // never becomes a zero-size blob - which rule A forbids spelling at all.
            if (storageBlockName == nullptr) return;
            BeforeReadOnlyVerb();

            MG_Pipe::MGPStorageBlockBinding record{};
            record.GlName = static_cast<Uint32>(program);
            record.Binding = static_cast<Uint32>(storageBlockBinding);
            record.ShaderCso = MG_Pipe::kMGPipeNullHandle;
            if (MG_State::pGLContext != nullptr) {
                const auto& programObject = MG_State::pGLContext->GetProgramObject(program);
                if (programObject) {
                    // The application may rebind a stage program before it is
                    // current or attached to a pipeline. A minted slot alone does
                    // not publish its archive; this verb must follow that birth.
                    programObject->JoinLinkAndSpirv();
                    Uint64 bytes = 0;
                    auto& emitter = MG_Pipe::MGPipeProgramEmitterInstance();
                    record.ShaderCso = emitter.AcquireShaderCso(*programObject, bytes);
                    emitter.EmitProgramBindings(*programObject, record.ShaderCso);
                }
            }
            // Size = strlen + 1: THE NUL TRAVELS (contract table 0's block-name row). The
            // decoder re-terminates into a bounded local and refuses a run whose last byte is
            // not NUL, so the two sides agree on where the name ends.
            const Uint64 nameBytes = static_cast<Uint64>(std::strlen(storageBlockName)) + 1ull;
            record.Name = session.StageBytes(storageBlockName, nameBytes);
            session.EmitAndWait(MG_Pipe::MGPWireOp::SetStorageBlockBinding, &record, sizeof(record),
                                nullptr, 0, nullptr, 0, nullptr);
        }

        // CLASS B - P5b package t2: the transform-feedback spans, the XFB object bind and the
        // tessellation patch parameter (MG_Remote/CONTRACT-P5B.md §2 t2).
        // =============================================================================
        //
        // SIX SLOTS, SIX ROWS, AND NOT ONE OF THEM STARTS A SHADER. Every one of the six is a
        // control call - it opens, closes, pauses, resumes or re-targets a capture span, or sets
        // the patch size the next tessellation draw uses - so each takes BeforeReadOnlyVerb(),
        // which is CONTRACT-P5B.md §4's rule for "a verb that reads buffers but starts no
        // shader" naming the XFB and patch controls by hand. The GPU-WRITE MARK FOR THE CAPTURE
        // TARGETS IS NOT TAKEN HERE and that is deliberate: it belongs at the END of the span,
        // after the record and before GLContext::EndTransformFeedback clears the live bindings,
        // which is exactly where b1 already put it (GL_Drawing.cpp's
        // MarkEndTransformFeedbackCaptureTargets). Taking it here as well would mark the same
        // buffers twice and taking it INSTEAD of there would mark nothing.
        //
        // P5f fe extends Begin with the immutable capture snapshot: program CSO handle,
        // XFB object lifetime id, and four buffer handle/range pairs. Buffer resource records
        // precede this verb through BeforeReadOnlyVerb; AcquireShaderCso below publishes the
        // program archive before Begin. Deferred driver Begin and End therefore need no
        // frontend program or binding-point lookup. set_stream_output_targets stays unused:
        // this state changes at the capture-span boundary, so it rides Begin itself.

        void EmitBeginTransformFeedback(GLenum primitiveMode) {
            VerbChannel session = ChannelFor("BeginTransformFeedback");
            BeforeReadOnlyVerb();

            MG_Pipe::MGPStreamOutputBegin record{};
            // The GL token verbatim (contract table 0's "GL enums on the wire"): the sink hands
            // it to the backend slot that takes it, and nothing between here and there reads it.
            record.PrimitiveMode = static_cast<Uint32>(primitiveMode);
            auto& context = *MG_State::pGLContext;
            record.LifetimeId = context.GetBoundTransformFeedbackLifetimeId();
            const auto& program = context.GetTransformFeedbackProgram();
            if (program) {
                Uint64 bytes = 0;
                record.CaptureProgram = MG_Pipe::MGPipeProgramEmitterInstance().AcquireShaderCso(*program, bytes);
            }
            static_assert(MG_State::GLState::GLContext::MAX_TRANSFORM_FEEDBACK_BUFFERS == 4);
            for (Uint i = 0; i < 4; ++i) {
                const auto& point = context.GetBufferBindingPoint(BufferTarget::TransformFeedback, i);
                const auto& buffer = point.GetBoundObject();
                if (!buffer) continue;
                const auto range = point.GetRange();
                const SizeT start = std::min(range.start, buffer->GetSize());
                const SizeT end = std::min(range.end, buffer->GetSize());
                record.Targets[i] = {MG_Pipe::MGPipeResourceTrackerInstance().Find(*buffer), start,
                                     end > start ? end - start : 0};
            }
            session.EmitAndWait(MG_Pipe::MGPWireOp::BeginStreamOutput, &record, sizeof(record),
                                nullptr, 0, nullptr, 0, nullptr);
        }

        void EmitEndTransformFeedback() {
            VerbChannel session = ChannelFor("EndTransformFeedback");
            BeforeReadOnlyVerb();

            MG_Pipe::MGPXfbAccounting record{};
            // THE ACCOUNTING IS THE CLIENT'S OWN AND IT IS INFORMATIONAL ON THIS SIDE OF THE
            // WIRE: end_stream_output's backend call takes no arguments, and the three numbers
            // are what the frontend has counted over this span (CONTRACT-P5B.md §2 t2, the
            // companions row). They travel because the row has carried them since P4a and
            // because they are what a server-side scatter would need when P9 lands one; the
            // sink today calls GL.EndTransformFeedback() and reads none of them. Read here,
            // BEFORE GLContext::EndTransformFeedback resets the counters at the call site.
            const auto& context = *MG_State::pGLContext;
            record.CapturedVertices = context.GetTransformFeedbackCapturedVertices();
            record.PrimitivesWritten = context.GetTransformFeedbackPrimitiveCounter();
            record.PrimitiveMode = static_cast<Uint32>(context.GetTransformFeedbackPrimitiveMode());
            session.EmitAndWait(MG_Pipe::MGPWireOp::EndStreamOutput, &record, sizeof(record),
                                nullptr, 0, nullptr, 0, nullptr);
        }

        void EmitPauseTransformFeedback() {
            VerbChannel session = ChannelFor("PauseTransformFeedback");
            BeforeReadOnlyVerb();

            // Reserved IS zero and the contract says so (MGPStreamOutputControl{Reserved = 0}).
            // The row exists to BE the verb boundary - the stamp the server puts up before the
            // sink runs - not to carry anything.
            MG_Pipe::MGPStreamOutputControl record{};
            record.Reserved = 0;
            session.EmitAndWait(MG_Pipe::MGPWireOp::PauseStreamOutput, &record, sizeof(record),
                                nullptr, 0, nullptr, 0, nullptr);
        }

        void EmitResumeTransformFeedback() {
            VerbChannel session = ChannelFor("ResumeTransformFeedback");
            BeforeReadOnlyVerb();

            MG_Pipe::MGPStreamOutputControl record{};
            record.Reserved = 0;
            session.EmitAndWait(MG_Pipe::MGPWireOp::ResumeStreamOutput, &record, sizeof(record),
                                nullptr, 0, nullptr, 0, nullptr);
        }

        void EmitBindTransformFeedback(GLuint name) {
            VerbChannel session = ChannelFor("BindTransformFeedback");
            BeforeReadOnlyVerb();

            MG_Pipe::MGPStreamOutputBind record{};
            // THE GL NAME IS NOT AN IDENTITY (ARCHITECTURE 4.2.1) and is carried anyway, because
            // it is the key the backend has always used: Espryt indexes its driver objects by it
            // (XfbImpl::g_xfbObjects[name], DirectGLES.cpp:1401) and generates the ES object on
            // first bind. Name 0 is the default object, which is why the field is not a handle.
            record.GlName = static_cast<Uint32>(name);
            // Beside it, the identity that WILL dispatch: the frontend's per-object lifetime id,
            // process-wide and never reused, which is what survives glGenTransformFeedbacks
            // recycling a name. Read AFTER GLContext::BindTransformFeedbackObject at the call
            // site, so it is the id of the object being bound and not of the previous one.
            record.LifetimeId = MG_State::pGLContext->GetBoundTransformFeedbackLifetimeId();
            session.EmitAndWait(MG_Pipe::MGPWireOp::BindStreamOutput, &record, sizeof(record),
                                nullptr, 0, nullptr, 0, nullptr);
        }

        void EmitPatchParameteri(GLenum pname, GLint value) {
            VerbChannel session = ChannelFor("PatchParameteri");
            BeforeReadOnlyVerb();

            MG_Pipe::MGPPatchParameter record{};
            // GL_PATCH_VERTICES is the only pname that reaches a backend slot - the frontend
            // answers GL_PATCH_DEFAULT_*_LEVEL itself and bakes those into the synthesized
            // control stage - and the frontend has already rejected every other pname with
            // INVALID_ENUM before this call (GL_Drawing.cpp's PatchParameteri). Carried verbatim
            // so the sink reproduces the call rather than a reading of it.
            record.Pname = static_cast<Uint32>(pname);
            record.Value = static_cast<Int32>(value);
            // set_patch_state (43) STILL TRAVELS, at the next validate, and that is not a
            // duplicate: it is the applier's working-block copy, this is the driver push Espryt
            // does AT THE CALL (DirectGLES.cpp:8760), and both pushes happen today on the
            // monolith path too (CONTRACT-P5B.md §2 t2).
            session.EmitAndWait(MG_Pipe::MGPWireOp::PatchParameter, &record, sizeof(record),
                                nullptr, 0, nullptr, 0, nullptr);
        }

        void EmitDeleteTransformFeedback(GLuint name) {
            VerbChannel session = ChannelFor("DeleteTransformFeedback");
            BeforeReadOnlyVerb();
            const auto& context = *MG_State::pGLContext;
            const MG_Pipe::MGPStreamOutputBind object{name, 0, context.GetTransformFeedbackLifetimeId(name)};
            session.EmitAndWait(MG_Pipe::MGPWireOp::DeleteStreamOutput, &object, sizeof(object),
                                nullptr, 0, nullptr, 0, nullptr);
            if (context.GetBoundTransformFeedbackName() == name) {
                const MG_Pipe::MGPStreamOutputBind fallback{0, 0, context.GetTransformFeedbackLifetimeId(0)};
                session.EmitAndWait(MG_Pipe::MGPWireOp::BindStreamOutput, &fallback, sizeof(fallback),
                                    nullptr, 0, nullptr, 0, nullptr);
            }
        }

    } // namespace

    // THE CONTROL'S OWN EVIDENCE LINE, and it is emitted per frame rather than at teardown
    // on purpose: a retrace that is killed by its own timeout, or whose library never runs
    // MobileGL::Destroy, would leave a teardown-only line absent and the control would then
    // have to accept a bare threshold failure - which is the thing R-16 forbids. One line
    // per Present, only while a knob is armed, is bounded by the frame count and present
    // whatever happens afterwards.
    void LogE2ControlLine(Uint64 frameOrdinal) {
        if (!g_dropClearEmission && !g_dropDrawEmission) return;
        MGLOG_W("MGPipe: E2 control armed - drop-draw=%d drop-clear=%d, %llu records dropped "
                "on the wire (draw=%llu clear=%llu), frame %llu",
                g_dropDrawEmission ? 1 : 0, g_dropClearEmission ? 1 : 0,
                static_cast<unsigned long long>(g_droppedDrawEmissions + g_droppedClearEmissions),
                static_cast<unsigned long long>(g_droppedDrawEmissions),
                static_cast<unsigned long long>(g_droppedClearEmissions),
                static_cast<unsigned long long>(frameOrdinal));
    }


    void ArmVerbControlKnobs() { ArmControlKnobs(); }

#if MOBILEGL_PIPE_VERIFY
    Bool MonolithPortApplyingForVerify() { return t_portApplying; }
#endif

    void SetVerbSessionResolver(VerbSessionResolver resolver) { g_verbSessionResolver = resolver; }

    void AssignVerbEmitters(MG_Backend::GlobalBackendFunctionsTable& table) {
        table.GL.DeleteTransformFeedback = &EmitDeleteTransformFeedback;
        table.GL.Clear = &EmitClear;
        table.GL.DrawArrays = &EmitDrawArrays;
        table.GL.DrawElements = &EmitDrawElements;
        table.GL.DrawElementsBaseVertex = &EmitDrawElementsBaseVertex;
        table.GL.DrawRangeElements = &EmitDrawRangeElements;
        table.GL.DrawRangeElementsBaseVertex = &EmitDrawRangeElementsBaseVertex;
        table.GL.DrawElementsInstanced = &EmitDrawElementsInstanced;
        table.GL.DrawElementsInstancedBaseVertex = &EmitDrawElementsInstancedBaseVertex;
        table.GL.DrawElementsInstancedBaseInstance = &EmitDrawElementsInstancedBaseInstance;
        table.GL.DrawArraysInstanced = &EmitDrawArraysInstanced;
        table.GL.DrawArraysInstancedBaseInstance = &EmitDrawArraysInstancedBaseInstance;
        table.GL.MultiDrawArrays = &EmitMultiDrawArrays;
        table.GL.MultiDrawElements = &EmitMultiDrawElements;
        table.GL.MultiDrawElementsBaseVertex = &EmitMultiDrawElementsBaseVertex;
        table.GL.DrawArraysIndirect = &EmitDrawArraysIndirect;
        table.GL.DrawElementsIndirect = &EmitDrawElementsIndirect;
        table.GL.MultiDrawArraysIndirect = &EmitMultiDrawArraysIndirect;
        table.GL.MultiDrawElementsIndirect = &EmitMultiDrawElementsIndirect;
        table.GL.MultiDrawArraysIndirectCount = &EmitMultiDrawArraysIndirectCount;
        table.GL.MultiDrawElementsIndirectCount = &EmitMultiDrawElementsIndirectCount;
        table.GL.ReadPixels = &EmitReadPixels;
        table.GL.BlitFramebuffer = &EmitBlitFramebuffer;
        table.GL.BlitNamedFramebuffer = &EmitBlitNamedFramebuffer;
        table.GL.BeginTransformFeedback = &EmitBeginTransformFeedback;
        table.GL.EndTransformFeedback = &EmitEndTransformFeedback;
        table.GL.PauseTransformFeedback = &EmitPauseTransformFeedback;
        table.GL.ResumeTransformFeedback = &EmitResumeTransformFeedback;
        table.GL.BindTransformFeedback = &EmitBindTransformFeedback;
        table.GL.PatchParameteri = &EmitPatchParameteri;
        table.GL.ClearBufferfi = &EmitClearBufferfi;
        table.GL.ClearBufferfv = &EmitClearBufferfv;
        table.GL.ClearBufferiv = &EmitClearBufferiv;
        table.GL.ClearBufferuiv = &EmitClearBufferuiv;
        table.GL.ClearNamedFramebufferfi = &EmitClearNamedFramebufferfi;
        table.GL.ClearNamedFramebufferfv = &EmitClearNamedFramebufferfv;
        table.GL.ClearNamedFramebufferiv = &EmitClearNamedFramebufferiv;
        table.GL.ClearNamedFramebufferuiv = &EmitClearNamedFramebufferuiv;
        table.GL.CopyTexImage2D = &EmitCopyTexImage2D;
        table.GL.CopyTexSubImage2D = &EmitCopyTexSubImage2D;
        table.GL.GenerateMipmap = &EmitGenerateMipmap;
        table.GL.GetTexImage = &EmitGetTexImage;
        table.GL.GetTextureImage = &EmitGetTextureImage;
        table.GL.BindImageTexture = &EmitBindImageTexture;
        table.GL.DispatchCompute = &EmitDispatchCompute;
        table.GL.DispatchComputeIndirect = &EmitDispatchComputeIndirect;
        table.GL.MemoryBarrier = &EmitMemoryBarrier;
        table.GL.MemoryBarrierByRegion = &EmitMemoryBarrierByRegion;
        table.GL.CopyImageSubData = &EmitCopyImageSubData;
        table.GL.ShaderStorageBlockBinding = &EmitShaderStorageBlockBinding;
        table.GL.DrawElementsInstancedBaseVertexBaseInstance = &EmitDrawElementsInstancedBaseVertexBaseInstance;
    }

    void InstallMonolithVerbPort(MG_Backend::GlobalBackendFunctionsTable& table) {
        // P13 W4a: the draw and dispatch families. Later W4 steps add their verbs here AND to
        // ApplyOnMonolithPort's switch, in the same commit.
        table.GL.DrawArrays = &EmitDrawArrays;
        table.GL.DrawElements = &EmitDrawElements;
        table.GL.DrawElementsBaseVertex = &EmitDrawElementsBaseVertex;
        table.GL.DrawRangeElements = &EmitDrawRangeElements;
        table.GL.DrawRangeElementsBaseVertex = &EmitDrawRangeElementsBaseVertex;
        table.GL.DrawElementsInstanced = &EmitDrawElementsInstanced;
        table.GL.DrawElementsInstancedBaseVertex = &EmitDrawElementsInstancedBaseVertex;
        table.GL.DrawElementsInstancedBaseInstance = &EmitDrawElementsInstancedBaseInstance;
        table.GL.DrawElementsInstancedBaseVertexBaseInstance = &EmitDrawElementsInstancedBaseVertexBaseInstance;
        table.GL.DrawArraysInstanced = &EmitDrawArraysInstanced;
        table.GL.DrawArraysInstancedBaseInstance = &EmitDrawArraysInstancedBaseInstance;
        table.GL.MultiDrawArrays = &EmitMultiDrawArrays;
        table.GL.MultiDrawElements = &EmitMultiDrawElements;
        table.GL.MultiDrawElementsBaseVertex = &EmitMultiDrawElementsBaseVertex;
        table.GL.DrawArraysIndirect = &EmitDrawArraysIndirect;
        table.GL.DrawElementsIndirect = &EmitDrawElementsIndirect;
        table.GL.MultiDrawArraysIndirect = &EmitMultiDrawArraysIndirect;
        table.GL.MultiDrawElementsIndirect = &EmitMultiDrawElementsIndirect;
        table.GL.MultiDrawArraysIndirectCount = &EmitMultiDrawArraysIndirectCount;
        table.GL.MultiDrawElementsIndirectCount = &EmitMultiDrawElementsIndirectCount;
        table.GL.DispatchCompute = &EmitDispatchCompute;
        table.GL.DispatchComputeIndirect = &EmitDispatchComputeIndirect;
        // P13 W4b: the texture family - its record arm resolves the texture from the verb's own
        // handle (VerbMipRes, VerbCopyTexDst, the copy's two endpoints).
        table.GL.GenerateMipmap = &EmitGenerateMipmap;
        table.GL.CopyTexImage2D = &EmitCopyTexImage2D;
        table.GL.CopyTexSubImage2D = &EmitCopyTexSubImage2D;
        table.GL.CopyImageSubData = &EmitCopyImageSubData;
        table.GL.ReadPixels = &EmitReadPixels;
        table.GL.GetTexImage = &EmitGetTexImage;
        table.GL.GetTextureImage = &EmitGetTextureImage;
        // P13 W4c: framebuffer verbs name their framebuffers by handle (MGPClear::Fbo, the blit's
        // pair), image binds and storage-block bindings carry their objects' handles.
        table.GL.Clear = &EmitClear;
        table.GL.ClearBufferfi = &EmitClearBufferfi;
        table.GL.ClearBufferfv = &EmitClearBufferfv;
        table.GL.ClearBufferiv = &EmitClearBufferiv;
        table.GL.ClearBufferuiv = &EmitClearBufferuiv;
        table.GL.ClearNamedFramebufferfi = &EmitClearNamedFramebufferfi;
        table.GL.ClearNamedFramebufferfv = &EmitClearNamedFramebufferfv;
        table.GL.ClearNamedFramebufferiv = &EmitClearNamedFramebufferiv;
        table.GL.ClearNamedFramebufferuiv = &EmitClearNamedFramebufferuiv;
        table.GL.BlitFramebuffer = &EmitBlitFramebuffer;
        table.GL.BlitNamedFramebuffer = &EmitBlitNamedFramebuffer;
        table.GL.BindImageTexture = &EmitBindImageTexture;
        table.GL.ShaderStorageBlockBinding = &EmitShaderStorageBlockBinding;
        // The XFB family: the span's capture program and targets ride Begin; the object is named
        // by lifetime id. It follows the program family because Begin names the capture program
        // by its shader CSO, whose archive monolith records carry from W4c.
        table.GL.BeginTransformFeedback = &EmitBeginTransformFeedback;
        table.GL.EndTransformFeedback = &EmitEndTransformFeedback;
        table.GL.PauseTransformFeedback = &EmitPauseTransformFeedback;
        table.GL.ResumeTransformFeedback = &EmitResumeTransformFeedback;
        table.GL.BindTransformFeedback = &EmitBindTransformFeedback;
        table.GL.DeleteTransformFeedback = &EmitDeleteTransformFeedback;
        g_monolithVerbPort = true;
    }

    Bool MonolithVerbPortInstalled() { return g_monolithVerbPort; }

    void SetDropClearEmissionForNegativeControl(Bool drop) { g_dropClearEmission = drop; }
    Uint64 DroppedClearEmissions() { return g_droppedClearEmissions; }

    void SetDropDrawEmissionForNegativeControl(Bool drop) { g_dropDrawEmission = drop; }
    Uint64 DroppedDrawEmissions() { return g_droppedDrawEmissions; }

    Bool ReadbackPackStateIsTightForTest(GLsizei width, Uint64 bytesPerPixel,
                                         const PixelStoreParameters& pack) {
        return ReadbackPackStateIsTight(width, bytesPerPixel, pack);
    }

    Uint64 TightReadbackByteCount(GLsizei width, GLsizei height, GLenum format, GLenum type) {
        return TightReadbackBytes(width, height, format, type);
    }

    // M2 / codex 11's predicate at namespace scope: the one RequireReadbackReplyComplete decides
    // on and the one the control drives. 0=OK / 1=DECLINED / 2=ERROR.
    Bool ReadbackReplyIsComplete(Int32 status, Uint64 replySize, Uint64 expected) {
        return status == MG_Pipe::MGPipeReplySink::kStatusOk && replySize == expected;
    }

    // g5-readback's plan (EmitTables.h, ReadbackBand). Pixels-per-reply first and the row test
    // against it, so no product here can overflow whatever the inputs: a unit control drives
    // this with Uint64 extremes, and the emitter with a GLsizei width.
    Bool PlanReadbackBands(Uint64 width, Uint64 height, Uint64 bytesPerPixel, Uint64 maxReplyBytes,
                           ReadbackBandPlan& plan) {
        if (width == 0 || height == 0 || bytesPerPixel == 0 || maxReplyBytes < bytesPerPixel) {
            return false;
        }
        const Uint64 pixelsPerReply = maxReplyBytes / bytesPerPixel; // >= 1
        if (width <= pixelsPerReply) {
            // floor(floor(cap / bpp) / width) == floor(cap / (bpp * width)): the most whole rows
            // one reply holds, and at least one because a row fits.
            plan.RowsPerBand = pixelsPerReply / width;
            plan.ColumnsPerBand = width;
        } else {
            // A SINGLE ROW IS LARGER THAN A REPLY (a >131071-pixel RGBA/FLOAT row at the 2 MiB
            // slot - wider than any attachment, but a legal glReadPixels whose in-bounds pixels
            // GL still defines). Each row is cut into pieces; one row per band keeps every piece
            // contiguous in the tight layout.
            plan.RowsPerBand = 1;
            plan.ColumnsPerBand = pixelsPerReply;
        }
        return true;
    }

    // ID-49's scatter, for one band of a banded read (g5-readback). The whole-read scatter below
    // is this function over the single band {0, height, 0, width}, so there is one copy of
    // 8.4.4's arithmetic and the control drives it.
    void ScatterReadbackBandIntoPackState(const void* band, void* destination, GLsizei width,
                                          const ReadbackBand& where, Uint64 bytesPerPixel,
                                          const PixelStoreParameters& pack) {
        if (band == nullptr || destination == nullptr || width <= 0 || where.Rows == 0 ||
            where.Columns == 0) {
            return;
        }
        const Uint64 rowPixels =
            pack.RowLength > 0 ? static_cast<Uint64>(pack.RowLength) : static_cast<Uint64>(width);
        const Uint64 alignment = pack.Alignment > 0 ? static_cast<Uint64>(pack.Alignment) : 1ull;
        const Uint64 strideBytes =
            ((rowPixels * bytesPerPixel + alignment - 1) / alignment) * alignment;
        // m6: the client re-derives GL 4.6 8.4.4's pack layout, so it honours GL_PACK_ROW_LENGTH
        // VERBATIM - including the ill-formed 0 < ROW_LENGTH < width, where the row stride is
        // narrower than a written row and consecutive rows overlap. GL leaves that case to the
        // implementation; the client reproduces exactly what the monolith backend's own scatter
        // would do with the same state rather than clamping, so the two arms stay byte-identical.
        // The fast path (ReadbackPackStateIsTight) already rejects any ROW_LENGTH != width, so
        // this only runs on the scatter path the application asked for.
        const Uint64 writtenPerRow = where.Columns * bytesPerPixel;
        // SKIP_IMAGES and IMAGE_HEIGHT ARE IGNORED (codex 6). glReadPixels is a 2-D read; GL
        // does not apply the image-level pack parameters to it, and the monolith conversion path
        // says so explicitly with honorPackImageParams=false (DirectGLES.cpp:10905). Applying
        // SKIP_IMAGES here shifted a read with SKIP_IMAGES=1 by a whole image and overran an
        // application buffer sized for exactly `height` rows. Only SKIP_ROWS and SKIP_PIXELS -
        // the 2-D skips - offset the first written byte; the band's own row and column then
        // offset it within the read, on the SAME stride, so a banded read writes exactly the
        // bytes the whole-read scatter would have, in the same row order.
        auto* out = static_cast<Uint8*>(destination) +
                    (static_cast<Uint64>(pack.SkipRows) + where.FirstRow) * strideBytes +
                    (static_cast<Uint64>(pack.SkipPixels) + where.FirstColumn) * bytesPerPixel;
        const auto* in = static_cast<const Uint8*>(band);
        for (Uint64 row = 0; row < where.Rows; ++row) {
            std::memcpy(out + row * strideBytes, in + row * writtenPerRow,
                        static_cast<SizeT>(writtenPerRow));
        }
    }

    // ID-49's scatter. Exported for the same reason as the refusal above: the control drives
    // THIS, which is what the emitter calls, rather than a second copy of 8.4.4's arithmetic.
    void ScatterTightReadbackIntoPackState(const void* tight, void* destination, GLsizei width,
                                           GLsizei height, Uint64 bytesPerPixel,
                                           const PixelStoreParameters& pack) {
        if (tight == nullptr || destination == nullptr || width <= 0 || height <= 0) return;
        ScatterReadbackBandIntoPackState(
            tight, destination, width,
            ReadbackBand{0, static_cast<Uint64>(height), 0, static_cast<Uint64>(width)},
            bytesPerPixel, pack);
    }

    // =============================================================================
    // P5b d1 - the draw family's record plan, at namespace scope so the unit cases drive
    // exactly what the emitters above call (R-16).
    // =============================================================================

    Uint8 RemoteIndexSizeFor(GLenum indexType) {
        switch (indexType) {
        case GL_UNSIGNED_BYTE: return 1;
        case GL_UNSIGNED_SHORT: return 2;
        case GL_UNSIGNED_INT: return 4;
        default: return 0;
        }
    }

    MG_Pipe::MGPDrawInfo PlanDrawInfo(GLenum mode, Uint8 indexSize, GLsizei instanceCount,
                                      GLuint baseInstance, Uint32 numDraws,
                                      const RemoteDrawBindings& bindings) {
        MG_Pipe::MGPDrawInfo info{};
        info.Mode = static_cast<Uint32>(mode);
        info.IndexSize = indexSize;
        // The restart state rides verbatim (informational in P5b: the backend's
        // ScopedRestartIndexSubstitution reads its own barrier-pulled copy and the index bytes
        // on its side). kDrawHasUserIndices / kDrawIsIndirect are added by the emission itself,
        // kDrawHasIndexRange by the two DrawRangeElements* callers.
        info.Flags = bindings.PrimitiveRestart ? static_cast<Uint8>(MG_Pipe::kDrawPrimitiveRestart) : 0;
        // P5e (vi), CONTRACT-P5E §2.1 (ii) / §5.1. On the wire BECAUSE the server needs it: the
        // barriered predicate is computed identically by both roles from the record alone, and
        // "does this draw fetch from client memory" is not derivable from anything else in it -
        // a null Res inside the vertex-buffer window is also what a DISABLED attribute below
        // the high-water mark publishes. Set for every draw shape, not just the array ones: a
        // glDrawElements can fetch its VERTICES from client memory too.
        if (bindings.ClientVertexArrays) info.Flags |= static_cast<Uint8>(MG_Pipe::kDrawClientArrays);
        // A negative count has already been refused by the frontend (INVALID_VALUE); 0 crosses
        // as 0 and draws nothing, which is what the driver does with it.
        info.InstanceCount = instanceCount > 0 ? static_cast<Uint32>(instanceCount) : 0u;
        info.StartInstance = baseInstance;
        info.RestartIndex = bindings.RestartIndex;
        info.DrawIdOffset = 0;
        // The handle beside the call (rule D): the VAO's element buffer for an indexed draw,
        // the same handle set_index_buffer (32) carried at validate; null for arrays and for a
        // client index array.
        info.IndexResource = (indexSize != 0 && bindings.ElementBufferBound) ? bindings.ElementBuffer
                                                                             : MG_Pipe::kMGPipeNullHandle;
        info.MinIndex = ~0u; // "unknown" (MGPipeTypes.h); the DrawRangeElements* callers fill it
        info.MaxIndex = ~0u;
        info.XfbCpuCapturedVertices = 0;
        info.NumDraws = numDraws;
        return info;
    }

    Bool PlanDrawRange(const RemoteDrawBindings& bindings, Uint8 indexSize, const void* indicesOrFirst,
                       GLsizei count, GLint baseVertex, MG_Pipe::MGPDrawRange& out) {
        out = MG_Pipe::MGPDrawRange{};
        out.Count = count > 0 ? static_cast<Uint32>(count) : 0u;
        if (indexSize == 0) {
            // Arrays: `first`, spelled through the pointer parameter so one function serves
            // both shapes. A negative first has already been refused by the frontend.
            const auto first = static_cast<std::intptr_t>(reinterpret_cast<std::uintptr_t>(indicesOrFirst));
            out.Start = first > 0 ? static_cast<Uint32>(first) : 0u;
            out.IndexBias = 0;
            return true;
        }
        out.IndexBias = baseVertex;
        if (!bindings.ElementBufferBound) {
            // A client index array: the emitter stages the bytes and the run starts at its
            // first index.
            out.Start = 0;
            return true;
        }
        // An element buffer: `indices` is a byte offset into it, and Start is that offset in
        // INDICES - the same arithmetic OnDrawVbo inverts (offset = Start * IndexSize). An
        // offset that is not a whole number of indices cannot be spelled and is the caller's
        // refusal, not a rounding.
        const auto offset = reinterpret_cast<std::uintptr_t>(indicesOrFirst);
        if (offset % indexSize != 0) return false;
        const std::uintptr_t start = offset / indexSize;
        if (start > 0xFFFFFFFFull) return false;
        out.Start = static_cast<Uint32>(start);
        return true;
    }

    MG_Pipe::MGPDrawIndirect PlanDrawIndirect(const RemoteDrawBindings& bindings, const void* indirect,
                                              GLsizei drawCount, GLsizei stride,
                                              GLintptr parameterOffset, Bool hasParameterBuffer) {
        MG_Pipe::MGPDrawIndirect block{};
        block.Buffer = bindings.DrawIndirectBuffer;
        block.ParameterBuffer = hasParameterBuffer ? bindings.ParameterBuffer : MG_Pipe::kMGPipeNullHandle;
        // The command byte offset the call passed as `indirect` (a bound GL_DRAW_INDIRECT_BUFFER
        // makes it an offset, never an address - the emitter refused the other case by name).
        block.Offset = static_cast<Uint64>(reinterpret_cast<std::uintptr_t>(indirect));
        block.ParameterOffset = hasParameterBuffer ? static_cast<Uint64>(parameterOffset) : 0u;
        // 0 = tightly packed, as GL spells it; the backend normalises. Negative counts and
        // strides have already been refused by the frontend.
        block.Stride = stride > 0 ? static_cast<Uint32>(stride) : 0u;
        block.DrawCount = drawCount > 0 ? static_cast<Uint32>(drawCount) : 0u;
        return block;
    }

} // namespace MobileGL::MG_Record
