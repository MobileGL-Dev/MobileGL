// MobileGL - MobileGL/MG_Remote/Client/EmitTables.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P5 package c1: the 71-slot emit table.
//
// THE PARTITION IS CONTRACT-P5.md §7's AND IS NOT RE-DERIVED HERE (R-15, ID-12):
//   class A  2 slots  answered locally from the caps mirror, never emitted, never Fatal
//   class B 24 slots  emitted (P5's five; P5b d1's nineteen draw slots, all on draw_vbo)
//   class C 45 slots  Fatal{UnmigratedVerb, "<slot>"}
// The three counts are static_asserted to sum to kRemoteEmitSlotCount below, so a slot that
// changes class without changing the arithmetic is a build break rather than a behaviour
// change nobody reviewed.
//
// THE PRE-VERB HOOKS RUN BEFORE THE RECORD, NEVER AFTER (b1, ID-18). PushPersistentMapsBeforeVerb
// publishes the bytes an application wrote through a coherent map with no API call at all, and
// MarkGpuWritesForDraw builds the conservative GPU-write set the client now owns. Both describe
// the work the record is ABOUT TO START, so a hook deferred past its own record is the C-1
// regression re-committed at the transport layer.

#include "EmitTables.h"

#include "ClientSession.h"
#include "GpuWritePending.h"
#include "PersistentMapTracker.h"

#include <MG_Util/Converters/GLToMG/TextureEnumConverter.h>
#include <MG_Util/Debug/Log.h>
#include <MG_Util/Metrics/PipeStats.h>
#include <MG_Util/Metrics/TextureMetrics.h>

#include <MG_State/GLState/BufferState/BufferObject.h>
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/VertexArrayState/VertexArrayObject.h>

// P5b d1: the handle a bound buffer already has (never minted here - the validate-time
// set_index_buffer / the buffer's own constructor did that), for MGPDrawInfo::IndexResource and
// the two indirect-buffer handles.
#include <MG_Impl/Pipe/ResourceTracker.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "WireTables.h"

namespace MobileGL::MG_Remote::Client {

    // The slot arithmetic, asserted rather than commented. GlobalBackendFunctionsTable is
    // GLFunctionsTable plus Present plus SetSwapInterval; GLFunctionsTable is 69 function
    // pointers plus one Bool (PrefersCpuXfbPrimitiveAccounting, BackendObject.h:274). A slot
    // added to either without a decision here is a build break, which is the point: R-4 forbids
    // a null slot, so a new slot needs an owner on the day it appears.
    static_assert(sizeof(MG_Backend::GlobalBackendFunctionsTable) ==
                      sizeof(MG_Backend::GLFunctionsTable) + 2 * sizeof(void (*)()),
                  "GlobalBackendFunctionsTable is no longer GLFunctionsTable + Present + SetSwapInterval");
    static_assert(sizeof(MG_Backend::GlobalBackendFunctionsTable) ==
                      kRemoteEmitSlotCount * sizeof(void (*)()) + sizeof(void (*)()),
                  "the emit table's 71 slots plus the packed Bool no longer describe the table");

    [[noreturn]] void UnmigratedVerbFatal(const char* slot) {
        // The same shape as MGPipeInputPoisonFatal (generated/PipeFilled.inc:407-413): names the
        // slot, live at every log level, aborts. Deliberately NOT MOBILEGL_ASSERT, which is
        // inert in an INFO build - and INFO is what every device lane runs.
        MGLOG_F("MGPipe: Fatal{UnmigratedVerb, \"%s\"}", slot);
        std::abort();
    }

    namespace {

        Bool g_dropClearEmission = false;
        Uint64 g_droppedClearEmissions = 0;
        Bool g_dropDrawEmission = false;
        Uint64 g_droppedDrawEmissions = 0;
        Uint64 g_presentOrdinal = 0;
        Uint64 g_publishedMaxRecordBytes = 0;

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

        // ---- the session, demanded rather than assumed --------------------------------
        //
        // Every class-B slot needs one. A null session here is NOT the monolith answer - the
        // monolith answer is that this table was never installed at all, because
        // MG_Backend::Init() only reaches BackendObject_Remote when the transport resolved. So
        // a null one is a Fatal by name and not a fall-through to the driver: a pass-through
        // slot is the "split lane ran monolith and went green" shape that every gate in this
        // phase exists to prevent (R-4).
        ClientSession& RequireSession(const char* slot) {
            RequireClientTablesInstalled(slot);
            ClientSession* session = ClientSession::Active();
            if (session == nullptr) {
                MGLOG_F("MGPipe: Fatal{NoClientSession, \"%s\"} - the remote emit table is "
                        "installed but no ClientSession is active. A slot may not fall through "
                        "to a driver this role does not have",
                        slot);
                std::abort();
            }
            return *session;
        }

        // The two hooks b1 wrote and deliberately left with no caller, because the call site is
        // this file's. ORDER: the push first (it produces resource_subdata records that must
        // precede the verb on SEG_CMD), then the mark walk, then the verb record.
        void BeforeDrawVerb() {
            PushPersistentMapsBeforeVerb();
            MarkGpuWritesForDraw();
        }

        // A verb that reads buffers but starts no shader: clear, blit, readback, present. The
        // push still has to run - a coherent map is read by the GPU on any of them - but there
        // is no shader that could write one, so no mark walk.
        void BeforeReadOnlyVerb() { PushPersistentMapsBeforeVerb(); }

        // =============================================================================
        // CLASS B - the five slots the verb census measured (CONTRACT-P5.md §7)
        // =============================================================================

        void EmitClear(GLbitfield mask) {
            ClientSession& session = RequireSession("Clear");
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
            record.Kind = kRemoteClearWhole;
            record.DrawBufferIndex = -1;
            record.BufferMask = static_cast<Uint32>(mask);
            record.ValueClass = 0;
            session.EmitAndWait(MG_Pipe::MGPWireOp::Clear, &record, sizeof(record), nullptr, 0,
                                nullptr, 0, nullptr);
        }

        void EmitDrawArrays(GLenum mode, GLint first, GLsizei count) {
            ClientSession& session = RequireSession("DrawArrays");
            BeforeDrawVerb();

            if (g_dropDrawEmission) {
                // E2's LOAD-BEARING negative control. Same shape as the clear drop and the same
                // rule: everything above still ran - the persistent-map push and the GPU-write
                // mark walk both happened - so the ONLY difference from the live arm is that
                // this frame's geometry never crossed the ring. A lane that still matches its
                // golden with this armed did not get its picture from the wire.
                ++g_droppedDrawEmissions;
                return;
            }

            MG_Pipe::MGPDrawInfo info{};
            info.Mode = static_cast<Uint32>(mode);
            info.IndexSize = 0; // arrays
            info.Flags = 0;     // NO kDrawHasUserIndices: the reduced path draws from a VBO
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
            session.EmitAndWait(MG_Pipe::MGPWireOp::DrawVbo, &info, sizeof(info), &range,
                                sizeof(range), nullptr, 0, nullptr);
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
        // WHAT IS REFUSED BY NAME, ID-57's shape (Fatal{UnmigratedVerb, "<slot>+<QUALIFIER>"}),
        // never rendered wrong and never fallen through to the driver (R-4):
        //   +CLIENT_INDICES    a glMultiDrawElements* with no element buffer bound: `indices[i]`
        //                      are drawcount separate client pointers and one span names one run;
        //                      P8's HostResolve.cpp flattens it. No measured workload has one.
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
            ClientSession& session = RequireSession(slot);
            BeforeDrawVerb();

            if (g_dropDrawEmission) {
                // E2's negative control covers EVERY draw record, not only DrawArrays: the
                // Minecraft traces are DrawElements frames, and a control that dropped only the
                // one P5 entry point would leave those lanes green with the wire disarmed.
                ++g_droppedDrawEmissions;
                return;
            }

            info.NumDraws = numDraws;
            Wire::WireTail tails[2] = {{ranges, static_cast<Uint64>(numDraws) * sizeof(MG_Pipe::MGPDrawRange)},
                                       {nullptr, 0}};
            Uint32 tailCount = 1;
            MG_Pipe::MGHostSpan span{};
            if (clientIndices != nullptr && clientIndexBytes != 0) {
                // The P8 resolve-on-client rule, applied: the bytes exist on the client only,
                // so the client stages them whole and the record names the run - Ptr = nullptr,
                // Seg = SEG_STAGE (rule B). The encoder runs all four honesty arms on the span
                // before it is published, and the sink resolves it through MGPipeHostBytes.
                const MG_Pipe::MGPBlobRef staged =
                    session.Encoder().StageBytes(clientIndices, clientIndexBytes);
                span.Ptr = nullptr;
                span.Seg = staged.Seg;
                span.Offset = staged.Offset;
                span.Size = staged.Size;
                info.Flags |= MG_Pipe::kDrawHasUserIndices;
                tails[1] = {&span, sizeof(span)};
                tailCount = 2;
            } else if (indirect != nullptr) {
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
            if (!bindings.ElementBufferBound) RefuseDrawByName(slot, "CLIENT_INDICES");
            const auto n = static_cast<Uint32>(drawcount);
            MG_Pipe::MGPDrawInfo info = PlanDrawInfo(mode, indexSize, 1, 0, n, bindings);
            Vector<MG_Pipe::MGPDrawRange>& ranges = MultiDrawScratch(n);
            for (Uint32 i = 0; i < n; ++i) {
                if (!PlanDrawRange(bindings, indexSize, indices[i], count[i],
                                   basevertex != nullptr ? basevertex[i] : 0, ranges[i])) {
                    RefuseDrawByName(slot, "INDEX_OFFSET");
                }
            }
            EmitDrawRecord(slot, info, ranges.data(), n, nullptr, 0, nullptr);
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
            ClientSession& session = RequireSession("BlitFramebuffer");
            BeforeReadOnlyVerb();

            MG_Pipe::MGPBlit record{};
            // Same reasoning as Clear's Fbo: the read and draw bindings are gPipeInputs', set
            // by MGP_FILL(BlitFramebuffer) at GL_Framebuffer.cpp:660 and held still by the
            // barrier. glBlitNamedFramebuffer, which DOES name two framebuffers, is class C.
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
                MGLOG_F("MGPipe: Fatal{UnsizedReadback, \"read_pixels\"} format=0x%04x type=0x%04x "
                        "- the client must declare MGPReadbackInfo::DstSize and cannot size this "
                        "pair; P5's reduced path reads RGBA/UNSIGNED_BYTE",
                        static_cast<unsigned>(format), static_cast<unsigned>(type));
                std::abort();
            }
            return static_cast<Uint64>(bytesPerPixel);
        }

        Uint64 TightReadbackBytes(GLsizei width, GLsizei height, GLenum format, GLenum type) {
            if (width <= 0 || height <= 0) return 0;
            return static_cast<Uint64>(width) * static_cast<Uint64>(height) *
                   ReadbackBytesPerPixel(format, type);
        }

        // M2 / codex 11: the reply the server posted is COMPLETE and OK. A short OK reply, and a
        // DECLINED or ERROR reply with a zero payload, both leave the destination full of stale
        // bytes; scattering or returning it is the silently truncated picture ID-47's own comment
        // says an SSIM comparison cannot see, arriving through the status field rather than
        // through truncation. `expected` is the record's own DstSize (CONTRACT-P5 row 23: the
        // reply's exact extent). The predicate is at namespace scope (below) and exposed for the
        // control, so R-16's "drive the production predicate" holds rather than a second copy of
        // the rule in the test.

        // The same predicate at the call site, with the named Fatal each failure mode owns.
        // status > expected cannot reach here: EmitAndWait already aborts Fatal{ReplyTooLarge} on
        // an oversize reply, so the only failures left are a wrong status or a SHORT one.
        void RequireReadbackReplyComplete(Int32 status, Uint64 replySize, Uint64 expected) {
            if (status == Wire::ReplySink::kStatusError) {
                MGLOG_F("MGPipe: Fatal{ReplyError, \"ReadPixels\"} - the readback answered ERROR; "
                        "the destination is left untouched rather than filled with stale bytes");
                std::abort();
            }
            if (status == Wire::ReplySink::kStatusDeclined) {
                MGLOG_F("MGPipe: Fatal{ReadbackDeclined, \"ReadPixels\"} - the server has no "
                        "GL.ReadPixels and DECLINED; a decline is a real answer for an acceptance "
                        "row (R-5) but a blocking readback has no pixels to return, so it is a "
                        "Fatal here rather than a buffer of stale bytes");
                std::abort();
            }
            if (replySize != expected) {
                MGLOG_F("MGPipe: Fatal{ReadbackReplyShort, \"ReadPixels %llu < %llu\"} - the OK "
                        "reply carried fewer bytes than the read's own DstSize (CONTRACT-P5 row "
                        "23's exact extent); the missing rows would otherwise be scattered as "
                        "whatever the destination held",
                        static_cast<unsigned long long>(replySize),
                        static_cast<unsigned long long>(expected));
                std::abort();
            }
        }

        void EmitReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format,
                            GLenum type, void* pixels) {
            ClientSession& session = RequireSession("ReadPixels");

            // ID-57 / M8: A PACK-PBO DESTINATION IS REFUSED BY NAME, BEFORE ANY EMISSION AND
            // BEFORE `pixels` IS TOUCHED. With GL_PIXEL_PACK_BUFFER bound, the frontend permits
            // `pixels` to be a byte OFFSET into that buffer, not an address (GL_Framebuffer.cpp:
            // 3055 aligns it to the type size, one byte for UNSIGNED_BYTE) - and this emitter has
            // no PBO branch: it sets DstOffset = 0, hands the offset to EmitAndWait as a host
            // buffer, and the reply is memcpy'd to CPU address <offset>. Under monolith the
            // backend maps the PBO and writes the reply into the buffer (unchanged). The real
            // split form - the server writes the reply into the buffer resource and the client
            // marks it GPU-written (b1's MarkReadPixelsPackBuffer becoming the producer contract
            // §3 names) - is a P6 ROADMAP item. In P5 it is class C's shape (R-4), refused here.
            if (MG_State::pGLContext != nullptr &&
                MG_State::pGLContext->GetBufferBindingSlot(::MobileGL::BufferTarget::PixelPack)
                    .GetBoundObject()) {
                UnmigratedVerbFatal("ReadPixels+PACK_BUFFER");
            }

            BeforeReadOnlyVerb();

            // THE PBO HALF IS b1's DESIGN AND b1 ALREADY WIRED ITS MARK, at
            // GL_Framebuffer.cpp:3109 - immediately after this table call returns, inside
            // ReadPixels_Backend itself. So this emitter deliberately does NOT call
            // MarkReadPixelsPackBuffer(): a second call there would be the "wire it twice"
            // shape, and the per-row counter b1's unit cases assert on would then count one
            // read as two. (In P5 the refusal above means no PBO read reaches here at all; the
            // note stays for the P6 form.)
            if (width <= 0 || height <= 0) return;
            const Uint64 bytesPerPixel = ReadbackBytesPerPixel(format, type);
            // ONE tight-size function for production AND the control (M3 / codex 10a). The first
            // cut computed this inline here while the test drove TightReadbackByteCount, so a
            // `+16` on the production line stayed green - the test observed a different number.
            // Now the number the server allocates and the number the test asserts come from the
            // same body.
            const Uint64 tight = TightReadbackBytes(width, height, format, type);

            MG_Pipe::MGPReadbackInfo info{};
            info.Res = MG_Pipe::kMGPipeNullHandle; // "the bound read surface answers"
            info.Box = MG_Pipe::MGPBox{x, y, 0, static_cast<Uint32>(width),
                                       static_cast<Uint32>(height), 1};
            info.Format = static_cast<Uint32>(format);
            info.Type = static_cast<Uint32>(type);
            info.Target = 0;
            info.Level = 0;
            info.DstOffset = 0;
            info.DstSize = tight;

            // CHECKED BEFORE THE EMISSION, not after the answer (ID-47), and through S1's
            // HELPER rather than a copy of it here. A reply bigger than a slot is Fatal on the
            // SERVER too, and s1 keeps that as the last line of defence - but it fires on the
            // apply thread with the record already on the wire, where all the client sees is a
            // hang. This one names the read, at the call site that knows what the read was.
            //
            // THE NUMBER IS ID-49's TIGHT EXTENT and not the packed one: the pack state never
            // crosses, so the answer that has to fit a slot is w*h*bytesPerPixel. The cap is
            // the pool's own, read live, so s1's growth of SEG_REPLY to 16 MiB / eight 2 MiB
            // slots needed no edit in this file - only the merge.
            session.RequireReadPixelsReplyFits(static_cast<Uint32>(width),
                                               static_cast<Uint32>(height),
                                               static_cast<Uint32>(format),
                                               static_cast<Uint32>(type), tight);

            PixelStoreParameters pack{};
            if (MG_State::pGLContext != nullptr) {
                pack = MG_State::pGLContext->GetPixelStoreParameters(/*isUnpack=*/false);
            }

            Int32 status = 0;
            Uint64 replySize = 0;
            if (ReadbackPackStateIsTight(width, bytesPerPixel, pack)) {
                // THE COMMON CASE, AND IT KEEPS THE ZERO-COPY. A neutral pack state means the
                // destination layout IS the tight layout, so the reply lands straight in the
                // application's buffer and there is no bounce at all. It is a fast path for the
                // SAME bytes, not a second rule: ScatterTightReadback below is a memcpy of the
                // whole run in exactly this case, and the control drives that function.
                session.EmitAndWait(MG_Pipe::MGPWireOp::ReadPixels, &info, sizeof(info), nullptr, 0,
                                    pixels, tight, &status, &replySize);
                // M2 / codex 11: an OK reply that arrived short, or a DECLINE/ERROR, must not be
                // handed back as pixels. The Fatal aborts before the application reads the buffer,
                // so the bytes EmitAndWait already copied into `pixels` are never observed.
                RequireReadbackReplyComplete(status, replySize, tight);
                return;
            }

            // The bounce is the price of the application having asked for a layout. It is the
            // tight size and never more, and it is freed before this returns - R-11's rule one
            // level out: nothing here outlives the call.
            Vector<Uint8> bounce(static_cast<SizeT>(tight));
            session.EmitAndWait(MG_Pipe::MGPWireOp::ReadPixels, &info, sizeof(info), nullptr, 0,
                                bounce.data(), tight, &status, &replySize);
            // BEFORE THE SCATTER, so a short or non-OK reply never reaches the application's
            // pointer at all (the bounce is the only thing that held the partial bytes).
            RequireReadbackReplyComplete(status, replySize, tight);
            ScatterTightReadbackIntoPackState(bounce.data(), pixels, width, height, bytesPerPixel,
                                              pack);
        }

        void EmitPresent() {
            ClientSession& session = RequireSession("Present");
            BeforeReadOnlyVerb();

            // ---- R-10's and R-9's readings, published BEFORE the present record ------------
            //
            // THE FRAME BOUNDARY IS THE RIGHT PLACE and the per-record path is the wrong one:
            // the encoder keeps all five as run totals, so this is five relaxed stores per
            // frame rather than five per record. Guarded by Enabled() like every other counting
            // site in the tree, so the cost with MOBILEGL_PIPE_STATS unset is a global load and
            // a predicted branch.
            //
            // AND IT IS BEFORE THE EmitAndWait BELOW, WHICH IS NOT A DETAIL. PipeStats::OnPresent
            // is called by the SERVER's Present - i.e. from inside the apply of the very record
            // this function is about to emit - so a publish placed after it lands one frame
            // late, and the FIRST summary line of every run then reads `maxrec=0 maxcap=0`.
            // Measured that way once: a zero that means "not published yet" is printed in the
            // same shape as a zero that means "nothing crossed", and the second one is a real
            // defect (an emit table that fell through to the driver). The Present record is 24
            // bytes and cannot be the maximum, so nothing is lost by reading one record early.
            if (MG_Util::PipeStats::Enabled()) {
                const Wire::PipeWireEncoder& encoder = session.Encoder();
                using MG_Util::PipeStats::Gauge;
                MG_Util::PipeStats::PublishGauge(Gauge::MaxRecordBytes, encoder.MaxRecordBytesSeen());
                MG_Util::PipeStats::PublishGauge(Gauge::MaxRecordBytesCap, encoder.MaxRecordBytesCap());
                MG_Util::PipeStats::PublishGauge(Gauge::RingWraps, encoder.CmdWraps());
                MG_Util::PipeStats::PublishGauge(Gauge::RingWrapPads, encoder.CmdWrapPads());
                MG_Util::PipeStats::PublishGauge(Gauge::RingWaits, encoder.StageReclaimWaits());

                // AND THE ROW, WHENEVER THE MAXIMUM MOVES. The summary line can carry the
                // number but not the name - MG_Util is below MG_Remote and has no WireOpName -
                // and the name is the actionable half: R-10 makes the integrator choose between
                // early chunking and a bigger ring, and that is a decision about a record
                // FAMILY. ClientSession::Stop prints the same pair at teardown, but a trace
                // replay never reaches it (measured: the OpenRA lane's library log ends mid-run
                // with no teardown line at all), so a stats-enabled run would otherwise publish
                // a size with no row. Emitted only when the maximum actually grows, so it is
                // bounded by the number of distinct maxima - five or six in a whole replay.
                if (encoder.MaxRecordBytesSeen() > g_publishedMaxRecordBytes) {
                    g_publishedMaxRecordBytes = encoder.MaxRecordBytesSeen();
                    MGLOG_I("MGPipe: wire ledger: new maximum record - maxrec=%llu "
                            "maxrecop=%s cap=%llu (R-10's proof obligation; P5 does not chunk)",
                            static_cast<unsigned long long>(g_publishedMaxRecordBytes),
                            encoder.MaxRecordOpName(),
                            static_cast<unsigned long long>(encoder.MaxRecordBytesCap()));
                }
            }

            MG_Pipe::MGPPresent record{};
            // FrameSerial 0 = "the server stamps its own". P5 has no client-side present credit
            // (MOBILEGL_IPC_PRESENT_CREDIT is P6's), so a client-minted serial would be a second
            // id space with no consumer.
            record.FrameSerial = 0;
            session.EmitAndWait(MG_Pipe::MGPWireOp::Present, &record, sizeof(record), nullptr, 0,
                                nullptr, 0, nullptr);

            // R-12's invalidation edge, drained at the one boundary every target crosses. A
            // second CapsSnapshot IS the invalidation; nothing else on the client can see that
            // the server re-ran InitCapabilities, because GLContext::GetCompileEnv()'s memo is
            // keyed on pActiveBackendObject.get() (Core.cpp:34) and that pointer never changes
            // under split.
            session.PumpControlPlane();

            ++g_presentOrdinal;
            LogE2ControlLine(g_presentOrdinal);
        }

        // =============================================================================
        // CLASS A - answered locally from the caps mirror (R-15). NO RECORD, EVER.
        // =============================================================================

        void AnswerGetIntegeri_v(GLenum target, GLuint index, GLint* data) {
            if (data == nullptr) return;
            const MG_Backend::DynamicBackendParameters& dynamic = CapsMirrorInstance().Dynamic();
            // The ONLY two indexed pnames the device owns; every other indexed pname names
            // frontend state and is answered in GL_Getter::GetIntegeri_v before any table is
            // consulted (BackendObject.h:196-205). The existing gate is
            // AdvertisedLimitsScenario.ComputeWorkGroupLimitsAreTheCapsBlocksAnswer, which pins
            // that this answer and the caps copy are ONE number.
            switch (target) {
            case GL_MAX_COMPUTE_WORK_GROUP_COUNT:
                if (index < 3) *data = static_cast<GLint>(dynamic.MaxComputeWorkGroupCount[index]);
                return;
            case GL_MAX_COMPUTE_WORK_GROUP_SIZE:
                if (index < 3) *data = static_cast<GLint>(dynamic.MaxComputeWorkGroupSize[index]);
                return;
            default:
                // Not a Fatal: the slot's own contract is "whatever pname the frontend has no
                // case for at all", and the monolith backends answer such a pname by leaving
                // the driver's own default in place. Answering a wrong number would be worse
                // than answering none.
                MGLOG_W_ONCE("MG_Remote client: GetIntegeri_v(0x%04x, %u) is not one of the two "
                             "device-owned indexed pnames and has no caps-mirror answer",
                             static_cast<unsigned>(target), static_cast<unsigned>(index));
                return;
            }
        }

        Bool AnswerIsTimerQuerySupported() {
            // A capability predicate, not a call. Today a null slot means COUNTER_BITS == 0
            // (GL_Query.cpp:792) - which is precisely the null check R-4 forbids, so it moves
            // here, to the bit the server published.
            return CapsMirrorInstance().HasCap(MG_Pipe::kCapTimerQuery);
        }

        // =============================================================================
        // CLASS C - Fatal{UnmigratedVerb}. 45 slots on this head: 44 in GLFunctionsTable +
        // SetSwapInterval (64 at the P5b contract commit; d1 v1 moved its 19 to class B).
        // =============================================================================
        //
        // PARTITIONED BY THE P5b PACKAGE THAT OWNS THE FLIP (MG_Remote/CONTRACT-P5B.md,
        // ~/w7/notes/p5b/BRIEF-P5B.md), so that four packages migrating in parallel edit four
        // DISJOINT lists and four DISJOINT counts rather than one list and one number. To flip a
        // slot a package (1) removes its X row from ITS list, (2) assigns the real emitter in
        // BuildRemoteEmitTable's class-B block, (3) raises ITS kEmittedSlots* by one. The
        // per-package ownership assertions below then still hold, the totals stay arithmetic,
        // and a slot that changes class without changing the arithmetic is a build break. The
        // X-macro shape is kept so the DEFINITION and the ASSIGNMENT cannot drift apart. Three
        // slots carry a body the macro cannot (DispatchCompute, DispatchComputeIndirect,
        // SetSwapInterval) and are written out by hand below.
        //
        //   d1   indexed / instanced / multi-draw / indirect draws -> draw_vbo (59), its
        //        kDrawIsIndirect tail and its kDrawHasUserIndices span
        //   i1   image bind, compute, barriers, copy-image, storage block -> bind_shader_image
        //        (72), launch_grid (60), memory_barrier (61), resource_copy_region (53),
        //        set_storage_block_binding (75)
        //   t2   the XFB spans and object bind, the patch parameter -> begin/end/pause/resume_
        //        stream_output (62..65), bind_stream_output (74), patch_parameter (73)
        //   f1   the clear family, the framebuffer-sourced copies, mips -> clear (57),
        //        copy_framebuffer_to_texture (76), generate_mipmap (54)
        //   tail the wave-3 remainder nothing measured: queries, syncs, the texture readbacks,
        //        the DSA blit, the swap interval (census-classC.md "static cross")

        // d1 v1 flipped all nineteen (DrawElements, DrawElementsBaseVertex, MultiDrawArrays,
        // MultiDrawElements, MultiDrawElementsBaseVertex, MultiDrawElementsIndirect,
        // MultiDrawArraysIndirect, MultiDrawElementsIndirectCount, MultiDrawArraysIndirectCount,
        // DrawRangeElementsBaseVertex, DrawRangeElements, the four DrawElementsInstanced*,
        // DrawArraysInstancedBaseInstance, DrawArraysInstanced, DrawElementsIndirect,
        // DrawArraysIndirect) to class B; the list is kept, empty, so the ownership assertion
        // below still reads 0 + 19 = 19 and a slot that fell back in would have to be added here.
#define MGR_UNMIGRATED_D1_SLOTS(X)

        // DispatchCompute and DispatchComputeIndirect are i1's too; they are hand-written below
        // because they carry b1's dispatch hook before the Fatal.
#define MGR_UNMIGRATED_I1_SLOTS(X)                                                                 \
    X(BindImageTexture, void, (GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum))           \
    X(CopyImageSubData, void,                                                                      \
      (const MG_Backend::CopyImageEndpoint&, GLenum, GLint, GLint, GLint, GLint,                   \
       const MG_Backend::CopyImageEndpoint&, GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, \
       GLsizei))                                                                                   \
    X(MemoryBarrier, void, (GLbitfield))                                                           \
    X(MemoryBarrierByRegion, void, (GLbitfield))                                                   \
    X(ShaderStorageBlockBinding, void, (GLuint, const GLchar*, GLuint))

#define MGR_UNMIGRATED_T2_SLOTS(X)                                                                 \
    X(PatchParameteri, void, (GLenum, GLint))                                                      \
    X(BeginTransformFeedback, void, (GLenum))                                                      \
    X(EndTransformFeedback, void, ())                                                              \
    X(PauseTransformFeedback, void, ())                                                            \
    X(ResumeTransformFeedback, void, ())                                                           \
    X(BindTransformFeedback, void, (GLuint))                                                       \
    X(DeleteTransformFeedback, void, (GLuint))

#define MGR_UNMIGRATED_F1_SLOTS(X)                                                                 \
    X(ClearBufferfi, void, (GLenum, GLint, GLfloat, GLint))                                        \
    X(ClearBufferfv, void, (GLenum, GLint, const GLfloat*))                                        \
    X(ClearBufferuiv, void, (GLenum, GLint, const GLuint*))                                        \
    X(ClearBufferiv, void, (GLenum, GLint, const GLint*))                                          \
    X(ClearNamedFramebufferfv, void,                                                               \
      (const SharedPtr<MG_State::GLState::FramebufferObject>&, GLenum, GLint, const GLfloat*))     \
    X(ClearNamedFramebufferfi, void,                                                               \
      (const SharedPtr<MG_State::GLState::FramebufferObject>&, GLenum, GLint, GLfloat, GLint))     \
    X(ClearNamedFramebufferiv, void,                                                               \
      (const SharedPtr<MG_State::GLState::FramebufferObject>&, GLenum, GLint, const GLint*))       \
    X(ClearNamedFramebufferuiv, void,                                                              \
      (const SharedPtr<MG_State::GLState::FramebufferObject>&, GLenum, GLint, const GLuint*))      \
    X(CopyTexImage2D, void, (GLenum, GLint, GLenum, GLint, GLint, GLsizei, GLsizei, GLint))        \
    X(CopyTexSubImage2D, void, (GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei))      \
    X(GenerateMipmap, void, (GLenum))

        // The wave-3 tail. SetSwapInterval is hand-written below (it is not a GL.* slot).
#define MGR_UNMIGRATED_TAIL_SLOTS(X)                                                               \
    X(BlitNamedFramebuffer, void,                                                                  \
      (const SharedPtr<MG_State::GLState::FramebufferObject>&,                                     \
       const SharedPtr<MG_State::GLState::FramebufferObject>&, GLint, GLint, GLint, GLint, GLint,  \
       GLint, GLint, GLint, GLbitfield, GLenum))                                                   \
    X(GetTexImage, void, (GLenum, GLint, GLenum, GLenum, GLvoid*))                                 \
    X(GetTextureImage, void,                                                                       \
      (const SharedPtr<MG_State::GLState::ITextureObject>&, TextureUploadTarget, GLint, GLenum,    \
       GLenum, GLsizei, GLvoid*))                                                                  \
    X(WaitSync, void, (MG_Backend::BackendSyncHandle, GLbitfield, GLuint64))                       \
    X(DeleteSync, void, (MG_Backend::BackendSyncHandle))                                           \
    X(EndTimeElapsedQuery, void, (MG_Backend::BackendQueryHandle))                                  \
    X(DeleteBackendQuery, void, (MG_Backend::BackendQueryHandle))                                   \
    X(EndOcclusionQuery, void, (MG_Backend::BackendQueryHandle))                                    \
    X(EndXfbPrimitivesQuery, void, (MG_Backend::BackendQueryHandle))

        // The non-void ones, kept apart only because the macro body differs: a [[noreturn]]
        // call is a complete body for a void slot and for a value-returning one alike, but a
        // compiler that does not see UnmigratedVerbFatal's attribute through the macro would
        // warn on the second. It does see it; they are split for readability. All ten are the
        // wave-3 tail.
#define MGR_UNMIGRATED_TAIL_VALUE_SLOTS(X)                                                         \
    X(FenceSync, MG_Backend::BackendSyncHandle, ())                                                \
    X(ClientWaitSync, GLenum, (MG_Backend::BackendSyncHandle, GLbitfield, GLuint64))               \
    X(GetSyncStatus, Bool, (MG_Backend::BackendSyncHandle))                                        \
    X(BeginTimeElapsedQuery, MG_Backend::BackendQueryHandle, ())                                   \
    X(QueryCounterTimestamp, MG_Backend::BackendQueryHandle, ())                                   \
    X(IsQueryResultAvailable, Bool, (MG_Backend::BackendQueryHandle))                               \
    X(GetQueryResult64, Bool, (MG_Backend::BackendQueryHandle, Bool, Uint64*))                      \
    X(BeginOcclusionQuery, MG_Backend::BackendQueryHandle, ())                                      \
    X(BeginXfbPrimitivesQuery, MG_Backend::BackendQueryHandle, (Bool))                              \
    X(GetGpuTimestampNs, Int64, ())

        // The union, for the places that want every class-C row at once (the definitions and
        // the assignments). A package never edits THIS; it edits its own list above.
#define MGR_UNMIGRATED_GL_SLOTS(X)                                                                 \
    MGR_UNMIGRATED_D1_SLOTS(X)                                                                     \
    MGR_UNMIGRATED_I1_SLOTS(X)                                                                     \
    MGR_UNMIGRATED_T2_SLOTS(X)                                                                     \
    MGR_UNMIGRATED_F1_SLOTS(X)                                                                     \
    MGR_UNMIGRATED_TAIL_SLOTS(X)
#define MGR_UNMIGRATED_GL_VALUE_SLOTS(X) MGR_UNMIGRATED_TAIL_VALUE_SLOTS(X)

#define MGR_DEFINE_UNMIGRATED(Name, Ret, Sig)                                                      \
    Ret Name##_Unmigrated Sig { UnmigratedVerbFatal(#Name); }

        MGR_UNMIGRATED_GL_SLOTS(MGR_DEFINE_UNMIGRATED)
        MGR_UNMIGRATED_GL_VALUE_SLOTS(MGR_DEFINE_UNMIGRATED)
#undef MGR_DEFINE_UNMIGRATED

        // THE TWO COMPUTE SLOTS CARRY b1's DISPATCH HOOK BEFORE THE FATAL, and this is stated
        // rather than hidden. MarkGpuWritesForDispatch() belongs immediately before the
        // dispatch record, and the dispatch record is class C until i1 lands - so the call site
        // is here, in the right place, and is UNREACHABLE-IN-EFFECT: the abort follows it. The
        // package that moves DispatchCompute into class B (i1: launch_grid, opcode 60) replaces
        // the Fatal and inherits a call site that is already correct rather than discovering
        // that the mark walk was never wired.
        void DispatchCompute_Unmigrated(GLuint, GLuint, GLuint) {
            PushPersistentMapsBeforeVerb();
            MarkGpuWritesForDispatch();
            UnmigratedVerbFatal("DispatchCompute");
        }
        void DispatchComputeIndirect_Unmigrated(GLintptr) {
            PushPersistentMapsBeforeVerb();
            MarkGpuWritesForDispatch();
            UnmigratedVerbFatal("DispatchComputeIndirect");
        }

        void SetSwapInterval_Unmigrated(Int) { UnmigratedVerbFatal("SetSwapInterval"); }

        // The counts, as arithmetic. MGR_COUNT_ONE expands to `+ 1` per row.
#define MGR_COUNT_ONE(Name, Ret, Sig) +1
        constexpr Uint32 kUnmigratedD1 = 0 MGR_UNMIGRATED_D1_SLOTS(MGR_COUNT_ONE);
        // + DispatchCompute, DispatchComputeIndirect, written out by hand.
        constexpr Uint32 kUnmigratedI1 = 0 MGR_UNMIGRATED_I1_SLOTS(MGR_COUNT_ONE) + 2;
        constexpr Uint32 kUnmigratedT2 = 0 MGR_UNMIGRATED_T2_SLOTS(MGR_COUNT_ONE);
        constexpr Uint32 kUnmigratedF1 = 0 MGR_UNMIGRATED_F1_SLOTS(MGR_COUNT_ONE);
        // + SetSwapInterval, written out by hand.
        constexpr Uint32 kUnmigratedTail =
            0 MGR_UNMIGRATED_TAIL_SLOTS(MGR_COUNT_ONE) MGR_UNMIGRATED_TAIL_VALUE_SLOTS(MGR_COUNT_ONE) + 1;
#undef MGR_COUNT_ONE
        constexpr Uint32 kUnmigratedSlots =
            kUnmigratedD1 + kUnmigratedI1 + kUnmigratedT2 + kUnmigratedF1 + kUnmigratedTail;

        // The emitted counts, PER OWNER. P5's five are c1's; each P5b package raises its own.
        constexpr Uint32 kEmittedSlotsP5 = 5; // Clear, DrawArrays, ReadPixels, Blit, Present
        constexpr Uint32 kEmittedSlotsD1 = 19; // the draw family, all on draw_vbo (d1 v1)
        constexpr Uint32 kEmittedSlotsI1 = 0;
        constexpr Uint32 kEmittedSlotsT2 = 0;
        constexpr Uint32 kEmittedSlotsF1 = 0;
        constexpr Uint32 kEmittedSlots =
            kEmittedSlotsP5 + kEmittedSlotsD1 + kEmittedSlotsI1 + kEmittedSlotsT2 + kEmittedSlotsF1;
        constexpr Uint32 kLocallyAnsweredSlots = 2; // GetIntegeri_v, IsTimerQuerySupported

        // EACH PACKAGE'S OWNERSHIP, PINNED. A package that flips a slot removes one row and
        // adds one to its emitted count; a package that touches another's list breaks the
        // other's line, not its own. The four numbers are the census's package tables plus the
        // unmeasured companions that share a wire row (BRIEF-P5B.md file-ownership table).
        static_assert(kUnmigratedD1 + kEmittedSlotsD1 == 19, "d1 owns the 19 draw slots");
        static_assert(kUnmigratedI1 + kEmittedSlotsI1 == 7, "i1 owns the 7 image/compute/barrier/copy/SSBO slots");
        static_assert(kUnmigratedT2 + kEmittedSlotsT2 == 7, "t2 owns the 7 XFB/tessellation slots");
        static_assert(kUnmigratedF1 + kEmittedSlotsF1 == 11, "f1 owns the 11 clear/copy/mip slots");
        static_assert(kUnmigratedTail == 20, "the wave-3 tail is 20 slots and no P5b package owns one");
        // 64 at the P5b contract commit; every P5b flip moves exactly one slot from C to B, so
        // the SUM is what stays pinned, and a package's flip changes its own count and nothing
        // else on this line.
        static_assert(kUnmigratedSlots + (kEmittedSlots - kEmittedSlotsP5) == 64,
                      "CONTRACT-P5.md §7 class C was 64 slots at the P5b contract commit; a P5b flip "
                      "moves one slot from C to B and the two counts must still sum to 64");
        static_assert(kLocallyAnsweredSlots + kEmittedSlots + kUnmigratedSlots == kRemoteEmitSlotCount,
                      "the three classes no longer partition the 71 slots");

        MG_Backend::GlobalBackendFunctionsTable BuildRemoteEmitTable() {
            ArmControlKnobs();
            MG_Backend::GlobalBackendFunctionsTable table{};

            // ---- class C first, so that a slot forgotten below stays Fatal rather than null.
            // Order matters for exactly this reason: if class B's assignment were first, a
            // typo in class C would leave a NULL slot, and a null slot is 91 potential null
            // calls with no diagnostic. This way the worst a mistake can do is name a verb
            // that was supposed to be emitted, loudly.
#define MGR_ASSIGN_UNMIGRATED(Name, Ret, Sig) table.GL.Name = &Name##_Unmigrated;
            MGR_UNMIGRATED_GL_SLOTS(MGR_ASSIGN_UNMIGRATED)
            MGR_UNMIGRATED_GL_VALUE_SLOTS(MGR_ASSIGN_UNMIGRATED)
#undef MGR_ASSIGN_UNMIGRATED
            table.GL.DispatchCompute = &DispatchCompute_Unmigrated;
            table.GL.DispatchComputeIndirect = &DispatchComputeIndirect_Unmigrated;
            table.SetSwapInterval = &SetSwapInterval_Unmigrated;

            // ---- class A
            table.GL.GetIntegeri_v = &AnswerGetIntegeri_v;
            table.GL.IsTimerQuerySupported = &AnswerIsTimerQuerySupported;
            // NOT A SLOT and not a verb: a Bool member of the table, whose one non-test client
            // reader is GL_Query.cpp:221. It does NOT ride inside MGPCaps::Dynamic - it is a
            // member of GLFunctionsTable, which is exactly the thing a split client never
            // receives - so it is answered from kCapCpuXfbPrimitiveAccounting.
            table.GL.PrefersCpuXfbPrimitiveAccounting =
                CapsMirrorInstance().PrefersCpuXfbPrimitiveAccounting();

            // ---- class B
            table.GL.Clear = &EmitClear;
            table.GL.DrawArrays = &EmitDrawArrays;
            // ---- P5b d1: the nineteen draw slots, all on draw_vbo (CONTRACT-P5B.md §2 d1)
            table.GL.DrawElements = &EmitDrawElements;
            table.GL.DrawElementsBaseVertex = &EmitDrawElementsBaseVertex;
            table.GL.DrawRangeElements = &EmitDrawRangeElements;
            table.GL.DrawRangeElementsBaseVertex = &EmitDrawRangeElementsBaseVertex;
            table.GL.DrawElementsInstanced = &EmitDrawElementsInstanced;
            table.GL.DrawElementsInstancedBaseVertex = &EmitDrawElementsInstancedBaseVertex;
            table.GL.DrawElementsInstancedBaseInstance = &EmitDrawElementsInstancedBaseInstance;
            table.GL.DrawElementsInstancedBaseVertexBaseInstance =
                &EmitDrawElementsInstancedBaseVertexBaseInstance;
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
            table.Present = &EmitPresent;

            return table;
        }

    } // namespace

    const MG_Backend::GlobalBackendFunctionsTable& RemoteEmitTable() {
        // Leaked at exit like every other MG_Remote singleton (ID-8): MG_Backend::Init()
        // copies it into gBackendFunctionsTable and MobileGL::Destroy() clears that copy from
        // an exit handler, by which point a static destructor here would already have run.
        static const MG_Backend::GlobalBackendFunctionsTable& table =
            *new MG_Backend::GlobalBackendFunctionsTable{BuildRemoteEmitTable()};
        return table;
    }

    Uint32 ImplementedVerbCount() { return kEmittedSlots; }
    Uint32 LocallyAnsweredSlotCount() { return kLocallyAnsweredSlots; }
    Uint32 UnmigratedSlotCount() { return kUnmigratedSlots; }

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
        return status == Wire::ReplySink::kStatusOk && replySize == expected;
    }

    // ID-49's scatter. Exported for the same reason as the refusal above: the control drives
    // THIS, which is what the emitter calls, rather than a second copy of 8.4.4's arithmetic.
    void ScatterTightReadbackIntoPackState(const void* tight, void* destination, GLsizei width,
                                           GLsizei height, Uint64 bytesPerPixel,
                                           const PixelStoreParameters& pack) {
        if (tight == nullptr || destination == nullptr || width <= 0 || height <= 0) return;
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
        const Uint64 writtenPerRow = static_cast<Uint64>(width) * bytesPerPixel;
        // SKIP_IMAGES and IMAGE_HEIGHT ARE IGNORED (codex 6). glReadPixels is a 2-D read; GL
        // does not apply the image-level pack parameters to it, and the monolith conversion path
        // says so explicitly with honorPackImageParams=false (DirectGLES.cpp:10905). Applying
        // SKIP_IMAGES here shifted a read with SKIP_IMAGES=1 by a whole image and overran an
        // application buffer sized for exactly `height` rows. Only SKIP_ROWS and SKIP_PIXELS -
        // the 2-D skips - offset the first written byte.
        auto* out = static_cast<Uint8*>(destination) +
                    static_cast<Uint64>(pack.SkipRows) * strideBytes +
                    static_cast<Uint64>(pack.SkipPixels) * bytesPerPixel;
        const auto* in = static_cast<const Uint8*>(tight);
        for (Uint64 row = 0; row < static_cast<Uint64>(height); ++row) {
            std::memcpy(out + row * strideBytes, in + row * writtenPerRow,
                        static_cast<SizeT>(writtenPerRow));
        }
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

} // namespace MobileGL::MG_Remote::Client
