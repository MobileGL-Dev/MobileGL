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
//   class B 54 slots emitted; class C 15 slots name their unmigrated verb.
// The three counts are static_asserted to sum to kRemoteEmitSlotCount below, so a slot that
// changes class without changing the arithmetic is a build break rather than a behaviour
// change nobody reviewed.
//
// P5b MOVES SLOTS FROM C TO B, ONE PACKAGE AT A TIME (MG_Remote/CONTRACT-P5B.md §7). The three
// numbers above are the partition AT THE P5b CONTRACT COMMIT and they are the ones the contract
// states; the arithmetic below is what the tree currently has, and the per-package ownership
// assertions say which package moved which slot. On this head t2 has landed: class B is 5 + 6
// and class C is 58.
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
#include <MG_Util/Converters/MGToGL/TextureEnumConverter.h>
#include <MG_Util/Debug/Log.h>
#include <MG_Util/Metrics/PipeStats.h>
#include <MG_Util/Metrics/TextureMetrics.h>

#include <MG_State/GLState/BufferState/BufferObject.h>
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/VertexArrayState/VertexArrayObject.h>
// P5b i1: the emitters below name a texture's, a buffer's and a program's HANDLE beside the GL
// arguments (rule D). The handle comes from the client's own slot allocator, which is
// MG_Impl/Pipe's - the same table MG_Impl/Pipe/ImageEmit.h's set_shader_images reads, so the
// two records name one identity rather than two.
#include <MG_Impl/Pipe/SlotAllocator.h>
#include <MG_State/GLState/ProgramState/ProgramObject.h>
#include <MG_State/GLState/TextureState/TextureState.h>

// P5b d1: the handle a bound buffer already has (never minted here - the validate-time
// set_index_buffer / the buffer's own constructor did that), for MGPDrawInfo::IndexResource and
// the two indirect-buffer handles.
#include <MG_Impl/Pipe/ResourceTracker.h>
#include <MG_Impl/Pipe/OwnedDrawInputs.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "WireTables.h"
#include <MG_Impl/Pipe/FramebufferEmit.h>
#include <MG_Impl/Pipe/PipeFill.h>
#include <MG_Impl/Pipe/TextureEmit.h>
#include <MG_Impl/Pipe/ProgramEmit.h>
#include <MG_Pipe/PipeMutation.h>

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

        // A verb that reads buffers but starts no shader: clear, blit, readback, present. The
        // push still has to run - a coherent map is read by the GPU on any of them - but there
        // is no shader that could write one, so no mark walk.
        void BeforeReadOnlyVerb() {
            PushPersistentMapsBeforeVerb();
            MG_Pipe::MGPipeDrainDeferredDestroys();
        }

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


        // ---- f1: verbatim clear/copy/mipmap records (CONTRACT-P5B §2) ----
        void EmitF1Clear(const char* slot, MG_Pipe::MGPipeHandle fbo, GLenum buffer,
                         GLint drawbuffer, Uint8 valueClass, const void* value,
                         GLfloat depth = 0, GLint stencil = 0) {
            auto& session = RequireSession(slot);
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
            auto& session = RequireSession(subImage ? "CopyTexSubImage2D" : "CopyTexImage2D");
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
            auto& session = RequireSession("GenerateMipmap");
            BeforeReadOnlyVerb();
            const auto& texture = F1BoundTexture(target);
            MG_Pipe::MGPMipPlan record{};
            record.Res = MG_Pipe::MGPipeTextureEmitterInstance().FindTexture(*texture);
            record.Target = static_cast<Uint16>(target);
            record.BaseLevel = texture->GetLevelRange().x();
            const auto* mipmap = dynamic_cast<const MG_State::GLState::TextureObjectMipmap*>(texture.get());
            record.LevelCount = mipmap ? mipmap->GetMipmapLevelCount() : 0;
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
            ClientSession& session = RequireSession(slot);
            PersistentMapTracker::Instance().PushDrawConsumers();
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
            Wire::WireTail tails[2] = {{ranges, static_cast<Uint64>(numDraws) * sizeof(MG_Pipe::MGPDrawRange)},
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
            ClientSession& session = RequireSession("BlitFramebuffer");
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
            ClientSession& session = RequireSession("BlitNamedFramebuffer");
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

        void ApplyReadbackByteSwap(void* pixels, Uint64 bytes, GLenum type,
                                   const PixelStoreParameters& pack) {
            if (!pack.SwapBytes || pixels == nullptr) return;
            const auto dataType = MG_Util::ConvertGLEnumToTexturePixelDataType(type);
            SizeT group = MG_Util::GetSizedTexturePixelDataTypeSize(dataType);
            if (group == 0) group = MG_Util::GetBaseTexturePixelDataTypeSize(dataType);
            // This packed depth/stencil type contains two independent 32-bit words.
            if (type == GL_FLOAT_32_UNSIGNED_INT_24_8_REV) group = 4;
            if (group <= 1) return;
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
            if (status != Wire::ReplySink::kStatusOk) {
                MGLOG_F("MGPipe: Fatal{ReplyStatusInvalid, \"ReadPixels\"} - unknown reply status %d", status);
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
            const auto pbo = MG_State::pGLContext != nullptr
                ? MG_State::pGLContext->GetBufferBindingSlot(::MobileGL::BufferTarget::PixelPack).GetBoundObject()
                : SharedPtr<MG_State::GLState::BufferObject>{};

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
            if (!pbo && ReadbackPackStateIsTight(width, bytesPerPixel, pack)) {
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
                ApplyReadbackByteSwap(pixels, tight, type, pack);
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
            ApplyReadbackByteSwap(bounce.data(), tight, type, pack);
            if (pbo) {
                // pixels is an offset, never a host pointer. Upload only the
                // requested rows so PACK padding and untouched bytes survive.
                const Uint64 alignment = std::max<Int>(pack.Alignment, 1);
                const Uint64 rowPixels = pack.RowLength > 0 ? pack.RowLength : width;
                const Uint64 stride = (rowPixels * bytesPerPixel + alignment - 1) / alignment * alignment;
                const Uint64 start = reinterpret_cast<SizeT>(pixels) + pack.SkipRows * stride +
                                     pack.SkipPixels * bytesPerPixel;
                const SizeT rowBytes = static_cast<SizeT>(width) * bytesPerPixel;
                for (Int y = 0; y < height; ++y)
                    pbo->UploadSubData({bounce.data() + y * rowBytes, rowBytes}, start + y * stride);
                return;
            }
            ScatterTightReadbackIntoPackState(bounce.data(), pixels, width, height, bytesPerPixel,
                                              pack);
        }

#include "TextureReadbackEmit.inc"

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

                // P5d round 3's wait ledger, CLIENT HALF, and it rides this frame boundary for
                // the same two reasons the five above it do: the producer keeps both as run
                // totals, so it is two relaxed stores per frame rather than two per wait, and
                // publishing before the present record means the first summary line of a run
                // carries real numbers instead of two zeroes that mean "not published yet".
                // The SERVER half is published by the apply thread from its own loop - rule E
                // does not let this thread read ServerLoop's counters.
                const Transport::SessionProducer& producer = session.Producer();
                MG_Util::PipeStats::PublishGauge(Gauge::ClientWaits, producer.Waits());
                MG_Util::PipeStats::PublishGauge(Gauge::ClientParks, producer.Parks());

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
            // P5e (ra, CONTRACT-P5E §1, §2.4). FrameSerial USED TO BE 0 - "the server stamps
            // its own" - and that was honest while nothing paced on it. It is now minted here,
            // 1-based, by AcquirePresentCredit, which also PAYS the credit: if this client
            // already has MOBILEGL_IPC_PRESENT_CREDIT presents in flight it parks until the
            // server's OnPresent returns one, and only then does it mint.
            //
            // THE CREDIT WAIT IS BEFORE THE ENCODE, WHICH IS NOT A DETAIL: EmitAndWait
            // reserves SEG_CMD bytes as its first act, so a client that encoded and then
            // parked would hold a ring reservation across a whole frame of server time. It
            // also drains SEG_EVENT on its way out, like every other wait (§2.6).
            //
            // With run-ahead disarmed this is a counter and nothing else, and the record below
            // travels exactly as it did - the present row's own barrier is the pacing there.
            record.FrameSerial = session.AcquirePresentCredit();
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
            ClientSession& session = RequireSession("BindImageTexture");

            MG_Pipe::MGPImageBind record{};
            // The unit's binding is the frontend's and has just been written by the caller, so
            // the texture this record names is the one the server's SyncImageTextureBinding
            // will pull for the same unit. Read from MG_State::pGLContext and NOT through
            // MGB_CTX: on this side of a split MGB_CTX is gPipeInputs, which is the SERVER's
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
            ClientSession& session = RequireSession("DispatchCompute");
            PersistentMapTracker::Instance().PushDrawConsumers();
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
            ClientSession& session = RequireSession("DispatchComputeIndirect");
            PersistentMapTracker::Instance().PushDrawConsumers();
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
            ClientSession& session = RequireSession("MemoryBarrier");
            MG_Pipe::MGPMemoryBarrier record{};
            record.Bits = static_cast<Uint32>(barriers);
            record.ByRegion = 0;
            session.EmitAndWait(MG_Pipe::MGPWireOp::MemoryBarrier, &record, sizeof(record), nullptr,
                                0, nullptr, 0, nullptr);
        }

        void EmitMemoryBarrierByRegion(GLbitfield barriers) {
            ClientSession& session = RequireSession("MemoryBarrierByRegion");
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
            ClientSession& session = RequireSession("CopyImageSubData");

            // ID-57's SHAPE: REFUSED BY NAME, BEFORE ANY EMISSION. GL 4.6 core 18.3.2 accepts
            // GL_RENDERBUFFER as either endpoint, and an endpoint is a sum type for exactly that
            // reason - but no sticky forward hands out a renderbuffer object, so the sink has no
            // way to rebuild one from a name, and none was measured. P7 is where the backend
            // takes handles and this arm becomes ordinary. The sink refuses the same shape by
            // the same name if a record ever reaches it (defence on both sides of one wire).
            if (src.IsRenderbuffer() || dst.IsRenderbuffer()) {
                UnmigratedVerbFatal("CopyImageSubData+RENDERBUFFER");
            }

            MG_Pipe::MGPCopyRegion record{};
            record.Src = PublishedTextureHandle(src.Texture);
            record.Dst = PublishedTextureHandle(dst.Texture);
            // The GL names beside the handles: the key MGB_CTX->GetTextureObject(name) takes on
            // the far side (a BARRIER-PULLED sticky forward, counted in `rsp`, retired by P7).
            record.SrcGlName =
                src.Texture ? static_cast<Uint32>(src.Texture->GetExternalIndex()) : 0u;
            record.DstGlName =
                dst.Texture ? static_cast<Uint32>(dst.Texture->GetExternalIndex()) : 0u;
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
            ClientSession& session = RequireSession("ShaderStorageBlockBinding");
            // The backend slot's own first line (DirectGLES.cpp:9201), kept here so a null name
            // never becomes a zero-size blob - which rule A forbids spelling at all.
            if (storageBlockName == nullptr) return;

            MG_Pipe::MGPStorageBlockBinding record{};
            record.GlName = static_cast<Uint32>(program);
            record.Binding = static_cast<Uint32>(storageBlockBinding);
            record.ShaderCso = MG_Pipe::kMGPipeNullHandle;
            if (MG_State::pGLContext != nullptr) {
                const auto& programObject = MG_State::pGLContext->GetProgramObject(program);
                if (programObject) {
                    record.ShaderCso = MG_Pipe::MGPipeSlots().FindByLifetimeId(
                        MG_Pipe::MGPipeKind::ShaderCso, programObject->GetLifetimeId());
                }
            }
            // Size = strlen + 1: THE NUL TRAVELS (contract table 0's block-name row). The
            // decoder re-terminates into a bounded local and refuses a run whose last byte is
            // not NUL, so the two sides agree on where the name ends.
            const Uint64 nameBytes = static_cast<Uint64>(std::strlen(storageBlockName)) + 1ull;
            record.Name = session.Encoder().StageBytes(storageBlockName, nameBytes);
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
            ClientSession& session = RequireSession("BeginTransformFeedback");
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
            ClientSession& session = RequireSession("EndTransformFeedback");
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
            ClientSession& session = RequireSession("PauseTransformFeedback");
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
            ClientSession& session = RequireSession("ResumeTransformFeedback");
            BeforeReadOnlyVerb();

            MG_Pipe::MGPStreamOutputControl record{};
            record.Reserved = 0;
            session.EmitAndWait(MG_Pipe::MGPWireOp::ResumeStreamOutput, &record, sizeof(record),
                                nullptr, 0, nullptr, 0, nullptr);
        }

        void EmitBindTransformFeedback(GLuint name) {
            ClientSession& session = RequireSession("BindTransformFeedback");
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
            ClientSession& session = RequireSession("PatchParameteri");
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
        // The frontend sees a local opaque token; neither this address nor the driver's
        // BackendSyncHandle is serialized. The Fence-kind slot allocator supplies wire identity.
        struct RemoteFenceProxy { MG_Pipe::MGPipeHandle Handle; };
        std::mutex g_fenceMutex;
        UnorderedMap<MG_Backend::BackendSyncHandle, UniquePtr<RemoteFenceProxy>> g_fenceProxies;

        MG_Pipe::MGPipeHandle FenceHandle(MG_Backend::BackendSyncHandle proxy) {
            const auto it = g_fenceProxies.find(proxy);
            if (it == g_fenceProxies.end())
                Wire::WireProtocolFatal("Fence.proxy", "unknown client fence proxy");
            return it->second->Handle;
        }

        MG_Backend::BackendSyncHandle EmitFenceSync() {
            ClientSession& session = RequireSession("FenceSync");
            const std::lock_guard<std::mutex> lock(g_fenceMutex);
            BeforeReadOnlyVerb();
            auto proxy = MakeUnique<RemoteFenceProxy>();
            proxy->Handle = MG_Pipe::MGPipeSlots().Allocate(MG_Pipe::MGPipeKind::Fence);
            const MG_Pipe::MGPHandleOnly desc{proxy->Handle, static_cast<Uint32>(MG_Pipe::MGPipeKind::Fence), 0};
            session.EmitAndWait(MG_Pipe::MGPWireOp::FenceCreate, &desc, sizeof(desc),
                                nullptr, 0, nullptr, 0, nullptr);
            auto* local = proxy.get();
            g_fenceProxies.emplace(local, std::move(proxy));
            return local;
        }

        Uint32 ReadFenceReply(ClientSession& session, MG_Pipe::MGPWireOp op,
                             const void* payload, Uint64 bytes) {
            Uint32 result = 0;
            Int32 status = Wire::ReplySink::kStatusError;
            Uint64 replyBytes = 0;
            session.EmitAndWait(op, payload, bytes, nullptr, 0, &result, sizeof(result),
                                &status, &replyBytes);
            if (status != Wire::ReplySink::kStatusOk || replyBytes != sizeof(result))
                Wire::WireProtocolFatal("Fence.reply", "missing or malformed sync result");
            return result;
        }

        GLenum EmitClientWaitSync(MG_Backend::BackendSyncHandle proxy, GLbitfield flags, GLuint64 timeout) {
            ClientSession& session = RequireSession("ClientWaitSync");
            const std::lock_guard<std::mutex> lock(g_fenceMutex);
            const MG_Pipe::MGPFenceWait request{FenceHandle(proxy), timeout, flags, 0};
            return ReadFenceReply(session, MG_Pipe::MGPWireOp::FenceWait, &request, sizeof(request));
        }

        Bool EmitGetSyncStatus(MG_Backend::BackendSyncHandle proxy) {
            ClientSession& session = RequireSession("GetSyncStatus");
            const std::lock_guard<std::mutex> lock(g_fenceMutex);
            const MG_Pipe::MGPHandleOnly desc{FenceHandle(proxy), static_cast<Uint32>(MG_Pipe::MGPipeKind::Fence), 0};
            const Uint32 result = ReadFenceReply(session, MG_Pipe::MGPWireOp::FenceStatus, &desc, sizeof(desc));
            if (result > 1) Wire::WireProtocolFatal("FenceStatus.reply", "status must be boolean");
            return result != 0;
        }

        void EmitWaitSync(MG_Backend::BackendSyncHandle proxy, GLbitfield flags, GLuint64 timeout) {
            ClientSession& session = RequireSession("WaitSync");
            const std::lock_guard<std::mutex> lock(g_fenceMutex);
            const MG_Pipe::MGPFenceWait request{FenceHandle(proxy), timeout, flags, 0};
            session.EmitAndWait(MG_Pipe::MGPWireOp::FenceWaitServer, &request, sizeof(request),
                                nullptr, 0, nullptr, 0, nullptr);
        }

        void EmitDeleteSync(MG_Backend::BackendSyncHandle proxy) {
            const std::lock_guard<std::mutex> lock(g_fenceMutex);
            const auto handle = FenceHandle(proxy);
            // MobileGL::Destroy stops the server BEFORE DestroyAllSyncObjects. Detach already
            // released those native objects on the apply thread; only the local proxy remains.
            if (auto* session = ClientSession::Active(); session != nullptr && session->Started()) {
                const MG_Pipe::MGPHandleOnly desc{handle, static_cast<Uint32>(MG_Pipe::MGPipeKind::Fence), 0};
                session->EmitAndWait(MG_Pipe::MGPWireOp::FenceDestroy, &desc, sizeof(desc),
                                     nullptr, 0, nullptr, 0, nullptr);
            }
            MG_Pipe::MGPipeSlots().Free(MG_Pipe::MGPipeKind::Fence, handle);
            g_fenceProxies.erase(proxy);
        }

#include "QueryEmit.inc"

        // CLASS C - each remaining slot names its first blocker.
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

        // i1 HAS LANDED: the list is EMPTY and all seven slots are class B (the five emitters
        // above plus the two compute ones). It is kept as an empty macro rather than deleted so
        // that MGR_UNMIGRATED_GL_SLOTS' union, kUnmigratedI1's arithmetic and the ownership
        // static_assert below all keep their shape - and so the next package to need a row here
        // (a P5b wave-3 image/compute slot) has the partition to put it in.
#define MGR_UNMIGRATED_I1_SLOTS(X)

        // t2 LANDED (CONTRACT-P5B.md §2 t2): the three measured slots - BeginTransformFeedback
        // (95 lane entries), PatchParameteri (43), BindTransformFeedback (2) - and the three
        // companions that share their rows are class B now and live in the block above.
        // DeleteTransformFeedback is the one that stays: it has NO ROW in P5b, by ruling and
        // not by omission (unmeasured; the driver object leaks on the server until P9's XFB
        // namespace work, and a bind of name 0 is what the backend does on delete of the bound
        // one, DirectGLES.cpp:1422). It therefore still aborts by its own name.
#define MGR_UNMIGRATED_T2_SLOTS(X)

#define MGR_UNMIGRATED_F1_SLOTS(X)

        // The wave-3 tail. SetSwapInterval is hand-written below (it is not a GL.* slot).
#define MGR_UNMIGRATED_TAIL_SLOTS(X)

        // The non-void ones, kept apart only because the macro body differs: a [[noreturn]]
        // call is a complete body for a void slot and for a value-returning one alike, but a
        // compiler that does not see UnmigratedVerbFatal's attribute through the macro would
        // warn on the second. It does see it; they are split for readability. All ten are the
        // wave-3 tail.
#define MGR_UNMIGRATED_TAIL_VALUE_SLOTS(X)

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

        // THE TWO COMPUTE SLOTS' class-C stubs are GONE (P5b i1): they carried b1's dispatch
        // hook before their Fatal so that the package flipping them would inherit a call site
        // that was already correct, and EmitDispatchCompute / EmitDispatchComputeIndirect above
        // are that inheritance - same two calls, same order, the record where the Fatal was.

        void SetSwapInterval_Unmigrated(Int) { UnmigratedVerbFatal("SetSwapInterval"); }

        // The counts, as arithmetic. MGR_COUNT_ONE expands to `+ 1` per row.
#define MGR_COUNT_ONE(Name, Ret, Sig) +1
        constexpr Uint32 kUnmigratedD1 = 0 MGR_UNMIGRATED_D1_SLOTS(MGR_COUNT_ONE);
        // i1 landed: the list is empty and the two hand-written compute stubs are gone with it.
        constexpr Uint32 kUnmigratedI1 = 0 MGR_UNMIGRATED_I1_SLOTS(MGR_COUNT_ONE);
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
        constexpr Uint32 kEmittedSlotsD1 = 19;
        constexpr Uint32 kEmittedSlotsI1 = 7;
        constexpr Uint32 kEmittedSlotsT2 = 7;
        constexpr Uint32 kEmittedSlotsF1 = 11;
        constexpr Uint32 kEmittedSlotsTail = 3; // BlitNamedFramebuffer, both texture readbacks
        constexpr Uint32 kEmittedSlotsSync = 5;
        constexpr Uint32 kEmittedSlotsQueries = 11;
        constexpr Uint32 kEmittedSlots =
            kEmittedSlotsP5 + kEmittedSlotsD1 + kEmittedSlotsI1 + kEmittedSlotsT2 + kEmittedSlotsF1 +
            kEmittedSlotsTail + kEmittedSlotsSync + kEmittedSlotsQueries;
        constexpr Uint32 kLocallyAnsweredSlots = 2; // GetIntegeri_v, IsTimerQuerySupported

        // EACH PACKAGE'S OWNERSHIP, PINNED. A package that flips a slot removes one row and
        // adds one to its emitted count; a package that touches another's list breaks the
        // other's line, not its own. The four numbers are the census's package tables plus the
        // unmeasured companions that share a wire row (BRIEF-P5B.md file-ownership table).
        static_assert(kUnmigratedD1 + kEmittedSlotsD1 == 19, "d1 owns the 19 draw slots");
        static_assert(kUnmigratedI1 + kEmittedSlotsI1 == 7, "i1 owns the 7 image/compute/barrier/copy/SSBO slots");
        static_assert(kUnmigratedT2 + kEmittedSlotsT2 == 7, "t2 owns the 7 XFB/tessellation slots");
        static_assert(kUnmigratedF1 + kEmittedSlotsF1 == 11, "f1 owns the 11 clear/copy/mip slots");
        static_assert(kUnmigratedTail + kEmittedSlotsTail + kEmittedSlotsSync + kEmittedSlotsQueries == 20,
                      "the original wave-3 tail owns 20 slots");
        static_assert(kUnmigratedSlots + kEmittedSlots == 69, "class B and C own 69 slots");
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
            table.SetSwapInterval = &SetSwapInterval_Unmigrated;

            table.GL.FenceSync = &EmitFenceSync;
            table.GL.ClientWaitSync = &EmitClientWaitSync;
            table.GL.GetSyncStatus = &EmitGetSyncStatus;
            table.GL.WaitSync = &EmitWaitSync;
            table.GL.DeleteSync = &EmitDeleteSync;
            table.GL.BeginTimeElapsedQuery = &EmitBeginTimeElapsedQuery;
            table.GL.EndTimeElapsedQuery = &EmitEndQuery;
            table.GL.QueryCounterTimestamp = &EmitQueryCounterTimestamp;
            table.GL.BeginOcclusionQuery = &EmitBeginOcclusionQuery;
            table.GL.EndOcclusionQuery = &EmitEndQuery;
            table.GL.BeginXfbPrimitivesQuery = &EmitBeginXfbPrimitivesQuery;
            table.GL.EndXfbPrimitivesQuery = &EmitEndQuery;
            table.GL.IsQueryResultAvailable = &EmitIsQueryResultAvailable;
            table.GL.GetQueryResult64 = &EmitGetQueryResult64;
            table.GL.DeleteBackendQuery = &EmitDeleteBackendQuery;
            table.GL.GetGpuTimestampNs = &EmitGetGpuTimestampNs;
            table.GL.DeleteTransformFeedback = &EmitDeleteTransformFeedback;

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
            table.GL.BlitNamedFramebuffer = &EmitBlitNamedFramebuffer;
            table.Present = &EmitPresent;
            // ---- class B, P5b t2. Assigned AFTER the class-C block above, which is what makes
            // the flip a single-line change per slot: the Fatal thunk is overwritten, and a slot
            // whose row is removed from MGR_UNMIGRATED_T2_SLOTS but not assigned here would be
            // NULL and caught by RemoteEmitTable.NoSlotIsNull rather than silently skipped.
            table.GL.BeginTransformFeedback = &EmitBeginTransformFeedback;
            table.GL.EndTransformFeedback = &EmitEndTransformFeedback;
            table.GL.PauseTransformFeedback = &EmitPauseTransformFeedback;
            table.GL.ResumeTransformFeedback = &EmitResumeTransformFeedback;
            table.GL.BindTransformFeedback = &EmitBindTransformFeedback;
            table.GL.PatchParameteri = &EmitPatchParameteri;

            // ---- f1 ----
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

            // ---- class B, P5b package i1 (kEmittedSlotsI1 = 7)
            table.GL.BindImageTexture = &EmitBindImageTexture;
            table.GL.DispatchCompute = &EmitDispatchCompute;
            table.GL.DispatchComputeIndirect = &EmitDispatchComputeIndirect;
            table.GL.MemoryBarrier = &EmitMemoryBarrier;
            table.GL.MemoryBarrierByRegion = &EmitMemoryBarrierByRegion;
            table.GL.CopyImageSubData = &EmitCopyImageSubData;
            table.GL.ShaderStorageBlockBinding = &EmitShaderStorageBlockBinding;

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

} // namespace MobileGL::MG_Remote::Client
