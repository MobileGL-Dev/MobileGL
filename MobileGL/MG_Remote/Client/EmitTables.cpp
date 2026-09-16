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
//   class B  5 slots  emitted
//   class C 64 slots  Fatal{UnmigratedVerb, "<slot>"}
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
#include <MG_Util/Metrics/TextureMetrics.h>

#include <MG_State/GLState/Core.h>

#include <cstdlib>
#include <cstring>

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

        // E2's control has to be armable from OUTSIDE the process that runs the replay, because
        // the statement it makes is about a trace lane and not about a unit case: "drop one
        // Clear emission and OpenRA's SSIM falls below 0.99". A recompile would make the control
        // arm against source text, which is ID-22(a)'s defect.
        //
        // READ WITH getenv RATHER THAN THROUGH MG_Config, DELIBERATELY AND TEMPORARILY. Config.h
        // is c0's and a new MOBILEGL_IPC_* knob goes through the integrator; this is a
        // NEGATIVE-CONTROL switch no operator may ever set, and it announces itself at warning
        // level every time it arms so it cannot be on by accident. Flagged for adoption into
        // IpcTable if the integrator wants it there.
        Bool ReadDropClearFromEnvironment() {
            const char* value = std::getenv("MOBILEGL_IPC_E2_DROP_CLEAR");
            const Bool armed = value != nullptr && value[0] == '1' && value[1] == '\0';
            if (armed) {
                MGLOG_W("MG_Remote client: MOBILEGL_IPC_E2_DROP_CLEAR=1 - E2's NEGATIVE CONTROL is "
                        "armed and every glClear will be DROPPED on the wire. This arm is expected "
                        "to fail its SSIM threshold; a lane that stays green with it set is not "
                        "going through the wire at all");
            }
            return armed;
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
            if (pack.SkipRows != 0 || pack.SkipPixels != 0 || pack.SkipImages != 0) return false;
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
                // E2's negative control. Everything above still ran, so the only difference
                // between this arm and the live one is the record - which is exactly the
                // statement "the picture comes from the wire" that E2 exists to prove.
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

        // ID-47's refusal, in its own function so the boundary pair can drive it without a
        // session. See EmitTables.h.
        void RefuseOversizeReadback(GLsizei width, GLsizei height, GLenum format, Uint64 bytes,
                                    Uint64 capacity) {
            if (bytes <= capacity) return;
            MGLOG_F("MGPipe: Fatal{ReplyTooLarge, \"ReadPixels %dx%d 0x%04x %llu > %llu\"} - P5 "
                    "does not chunk a readback (R-10) and must not truncate one; grow "
                    "MOBILEGL_IPC_REPLY_MB or read less",
                    static_cast<int>(width), static_cast<int>(height),
                    static_cast<unsigned>(format), static_cast<unsigned long long>(bytes),
                    static_cast<unsigned long long>(capacity));
            std::abort();
        }

        void EmitReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format,
                            GLenum type, void* pixels) {
            ClientSession& session = RequireSession("ReadPixels");
            BeforeReadOnlyVerb();

            // THE PBO HALF IS b1's DESIGN AND b1 ALREADY WIRED ITS MARK, at
            // GL_Framebuffer.cpp:3109 - immediately after this table call returns, inside
            // ReadPixels_Backend itself. So this emitter deliberately does NOT call
            // MarkReadPixelsPackBuffer(): a second call there would be the "wire it twice"
            // shape, and the per-row counter b1's unit cases assert on would then count one
            // read as two.
            if (width <= 0 || height <= 0) return;
            const Uint64 bytesPerPixel = ReadbackBytesPerPixel(format, type);
            const Uint64 tight = static_cast<Uint64>(width) * static_cast<Uint64>(height) * bytesPerPixel;

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

            // CHECKED BEFORE THE EMISSION, not after the answer (ID-47). A reply bigger than a
            // slot is Fatal on the SERVER, and a Fatal there is a dead apply thread with a
            // client parked in the barrier for ever, naming a byte count and not a read; here
            // it is one line naming the read. The capacity is read LIVE from the pool rather
            // than compared against a constant, so s1's growth of SEG_REPLY to 16 MiB / eight
            // 2 MiB slots needs no edit in this file.
            RefuseOversizeReadback(width, height, format, tight, session.MaxReplyBytes());

            PixelStoreParameters pack{};
            if (MG_State::pGLContext != nullptr) {
                pack = MG_State::pGLContext->GetPixelStoreParameters(/*isUnpack=*/false);
            }

            Int32 status = 0;
            if (ReadbackPackStateIsTight(width, bytesPerPixel, pack)) {
                // THE COMMON CASE, AND IT KEEPS THE ZERO-COPY. A neutral pack state means the
                // destination layout IS the tight layout, so the reply lands straight in the
                // application's buffer and there is no bounce at all. It is a fast path for the
                // SAME bytes, not a second rule: ScatterTightReadback below is a memcpy of the
                // whole run in exactly this case, and the control drives that function.
                session.EmitAndWait(MG_Pipe::MGPWireOp::ReadPixels, &info, sizeof(info), nullptr, 0,
                                    pixels, tight, &status);
                return;
            }

            // The bounce is the price of the application having asked for a layout. It is the
            // tight size and never more, and it is freed before this returns - R-11's rule one
            // level out: nothing here outlives the call.
            Vector<Uint8> bounce(static_cast<SizeT>(tight));
            session.EmitAndWait(MG_Pipe::MGPWireOp::ReadPixels, &info, sizeof(info), nullptr, 0,
                                bounce.data(), tight, &status);
            ScatterTightReadbackIntoPackState(bounce.data(), pixels, width, height, bytesPerPixel,
                                              pack);
        }

        void EmitPresent() {
            ClientSession& session = RequireSession("Present");
            BeforeReadOnlyVerb();

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
        // CLASS C - Fatal{UnmigratedVerb}. 64 slots: 63 in GLFunctionsTable + SetSwapInterval.
        // =============================================================================
        //
        // The list is an X-macro so the DEFINITION and the ASSIGNMENT cannot drift apart, and
        // so the count is arithmetic rather than a comment. Two of them carry a pre-verb hook
        // before the Fatal - see the note on DispatchCompute.

#define MGR_UNMIGRATED_GL_SLOTS(X)                                                                 \
    X(DrawElements, void, (GLenum, GLsizei, GLenum, const void*))                                  \
    X(DrawElementsBaseVertex, void, (GLenum, GLsizei, GLenum, const void*, GLint))                 \
    X(MultiDrawArrays, void, (GLenum, const GLint*, const GLsizei*, GLsizei))                      \
    X(MultiDrawElements, void, (GLenum, const GLsizei*, GLenum, const GLvoid* const*, GLsizei))    \
    X(MultiDrawElementsBaseVertex, void,                                                           \
      (GLenum, const GLsizei*, GLenum, const GLvoid* const*, GLsizei, const GLint*))               \
    X(MultiDrawElementsIndirect, void, (GLenum, GLenum, const void*, GLsizei, GLsizei))            \
    X(MultiDrawArraysIndirect, void, (GLenum, const void*, GLsizei, GLsizei))                      \
    X(MultiDrawElementsIndirectCount, void, (GLenum, GLenum, const void*, GLintptr, GLsizei, GLsizei)) \
    X(MultiDrawArraysIndirectCount, void, (GLenum, const void*, GLintptr, GLsizei, GLsizei))       \
    X(DrawRangeElementsBaseVertex, void,                                                           \
      (GLenum, GLuint, GLuint, GLsizei, GLenum, const void*, GLint))                               \
    X(DrawRangeElements, void, (GLenum, GLuint, GLuint, GLsizei, GLenum, const void*))             \
    X(DrawElementsInstancedBaseVertexBaseInstance, void,                                           \
      (GLenum, GLsizei, GLenum, const void*, GLsizei, GLint, GLuint))                              \
    X(DrawElementsInstancedBaseVertex, void, (GLenum, GLsizei, GLenum, const void*, GLsizei, GLint)) \
    X(DrawElementsInstancedBaseInstance, void,                                                     \
      (GLenum, GLsizei, GLenum, const void*, GLsizei, GLuint))                                     \
    X(DrawElementsInstanced, void, (GLenum, GLsizei, GLenum, const void*, GLsizei))                \
    X(DrawArraysInstancedBaseInstance, void, (GLenum, GLint, GLsizei, GLsizei, GLuint))            \
    X(DrawArraysInstanced, void, (GLenum, GLint, GLsizei, GLsizei))                                \
    X(DrawElementsIndirect, void, (GLenum, GLenum, const void*))                                   \
    X(DrawArraysIndirect, void, (GLenum, const void*))                                             \
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
    X(BlitNamedFramebuffer, void,                                                                  \
      (const SharedPtr<MG_State::GLState::FramebufferObject>&,                                     \
       const SharedPtr<MG_State::GLState::FramebufferObject>&, GLint, GLint, GLint, GLint, GLint,  \
       GLint, GLint, GLint, GLbitfield, GLenum))                                                   \
    X(CopyTexImage2D, void, (GLenum, GLint, GLenum, GLint, GLint, GLsizei, GLsizei, GLint))        \
    X(CopyTexSubImage2D, void, (GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei))      \
    X(CopyImageSubData, void,                                                                      \
      (const MG_Backend::CopyImageEndpoint&, GLenum, GLint, GLint, GLint, GLint,                   \
       const MG_Backend::CopyImageEndpoint&, GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, \
       GLsizei))                                                                                   \
    X(GenerateMipmap, void, (GLenum))                                                              \
    X(GetTexImage, void, (GLenum, GLint, GLenum, GLenum, GLvoid*))                                 \
    X(GetTextureImage, void,                                                                       \
      (const SharedPtr<MG_State::GLState::ITextureObject>&, TextureUploadTarget, GLint, GLenum,    \
       GLenum, GLsizei, GLvoid*))                                                                  \
    X(MemoryBarrier, void, (GLbitfield))                                                           \
    X(MemoryBarrierByRegion, void, (GLbitfield))                                                   \
    X(BindImageTexture, void, (GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum))           \
    X(ShaderStorageBlockBinding, void, (GLuint, const GLchar*, GLuint))                            \
    X(WaitSync, void, (MG_Backend::BackendSyncHandle, GLbitfield, GLuint64))                       \
    X(DeleteSync, void, (MG_Backend::BackendSyncHandle))                                           \
    X(EndTimeElapsedQuery, void, (MG_Backend::BackendQueryHandle))                                  \
    X(DeleteBackendQuery, void, (MG_Backend::BackendQueryHandle))                                   \
    X(EndOcclusionQuery, void, (MG_Backend::BackendQueryHandle))                                    \
    X(EndXfbPrimitivesQuery, void, (MG_Backend::BackendQueryHandle))                                \
    X(PatchParameteri, void, (GLenum, GLint))                                                      \
    X(BeginTransformFeedback, void, (GLenum))                                                      \
    X(EndTransformFeedback, void, ())                                                              \
    X(PauseTransformFeedback, void, ())                                                            \
    X(ResumeTransformFeedback, void, ())                                                           \
    X(BindTransformFeedback, void, (GLuint))                                                       \
    X(DeleteTransformFeedback, void, (GLuint))

        // The non-void ones, kept apart only because the macro body differs: a [[noreturn]]
        // call is a complete body for a void slot and for a value-returning one alike, but a
        // compiler that does not see UnmigratedVerbFatal's attribute through the macro would
        // warn on the second. It does see it; they are split for readability.
#define MGR_UNMIGRATED_GL_VALUE_SLOTS(X)                                                           \
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

#define MGR_DEFINE_UNMIGRATED(Name, Ret, Sig)                                                      \
    Ret Name##_Unmigrated Sig { UnmigratedVerbFatal(#Name); }

        MGR_UNMIGRATED_GL_SLOTS(MGR_DEFINE_UNMIGRATED)
        MGR_UNMIGRATED_GL_VALUE_SLOTS(MGR_DEFINE_UNMIGRATED)
#undef MGR_DEFINE_UNMIGRATED

        // THE TWO COMPUTE SLOTS CARRY b1's DISPATCH HOOK BEFORE THE FATAL, and this is stated
        // rather than hidden. MarkGpuWritesForDispatch() belongs immediately before the
        // dispatch record, and the dispatch record is class C in P5 - so the call site is here,
        // in the right place, and is UNREACHABLE-IN-EFFECT: the abort follows it. There is no
        // gate on it and this file says so; the phase that moves DispatchCompute into class B
        // replaces the Fatal and inherits a call site that is already correct rather than
        // discovering that the mark walk was never wired.
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
        constexpr Uint32 kUnmigratedListedSlots =
            0 MGR_UNMIGRATED_GL_SLOTS(MGR_COUNT_ONE) MGR_UNMIGRATED_GL_VALUE_SLOTS(MGR_COUNT_ONE);
#undef MGR_COUNT_ONE
        // + DispatchCompute, DispatchComputeIndirect, SetSwapInterval, written out by hand
        // because they carry a body the macro cannot.
        constexpr Uint32 kUnmigratedSlots = kUnmigratedListedSlots + 3;
        constexpr Uint32 kLocallyAnsweredSlots = 2; // GetIntegeri_v, IsTimerQuerySupported
        constexpr Uint32 kEmittedSlots = 5;         // Clear, DrawArrays, ReadPixels, Blit, Present

        static_assert(kUnmigratedSlots == 64, "CONTRACT-P5.md §7 class C is 64 slots");
        static_assert(kLocallyAnsweredSlots + kEmittedSlots + kUnmigratedSlots == kRemoteEmitSlotCount,
                      "the three classes no longer partition the 71 slots");

        MG_Backend::GlobalBackendFunctionsTable BuildRemoteEmitTable() {
            g_dropClearEmission = ReadDropClearFromEnvironment();
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

    // ID-47's refusal, exported so the boundary pair drives THE EMITTER'S OWN decision rather
    // than a copy of it. One line, because the arithmetic that produced `bytes` is
    // TightReadbackBytes' and the capacity is the pool's - this function only decides.
    void RefuseReadbackLargerThanTheReplySlot(GLsizei width, GLsizei height, GLenum format,
                                              Uint64 bytes, Uint64 capacity) {
        RefuseOversizeReadback(width, height, format, bytes, capacity);
    }

    Bool ReadbackPackStateIsTightForTest(GLsizei width, Uint64 bytesPerPixel,
                                         const PixelStoreParameters& pack) {
        return ReadbackPackStateIsTight(width, bytesPerPixel, pack);
    }

    Uint64 TightReadbackByteCount(GLsizei width, GLsizei height, GLenum format, GLenum type) {
        return TightReadbackBytes(width, height, format, type);
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
        const Uint64 writtenPerRow = static_cast<Uint64>(width) * bytesPerPixel;
        // SKIP_IMAGES is in the parameter set and is meaningless for a 2D read, so it is
        // applied as GL defines it (whole images of ROW_LENGTH x IMAGE_HEIGHT) rather than
        // ignored - ignoring a non-zero one would silently write over the application's first
        // image.
        const Uint64 imageRows =
            pack.ImageHeight > 0 ? static_cast<Uint64>(pack.ImageHeight) : static_cast<Uint64>(height);
        auto* out = static_cast<Uint8*>(destination) +
                    static_cast<Uint64>(pack.SkipImages) * imageRows * strideBytes +
                    static_cast<Uint64>(pack.SkipRows) * strideBytes +
                    static_cast<Uint64>(pack.SkipPixels) * bytesPerPixel;
        const auto* in = static_cast<const Uint8*>(tight);
        for (Uint64 row = 0; row < static_cast<Uint64>(height); ++row) {
            std::memcpy(out + row * strideBytes, in + row * writtenPerRow,
                        static_cast<SizeT>(writtenPerRow));
        }
    }

} // namespace MobileGL::MG_Remote::Client
