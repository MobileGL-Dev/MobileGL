// MobileGL - MobileGL/MG_Backend/Init.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "BackendObjects.h"
#include <Config.h>
#include <MG_Util/BackendLoaders/OpenGL/Loader.h>
#include <MG_Util/Converters/MGToStr/GLExtensionConverter.h>

#if MOBILEGL_BUILD_DISAGGREGATED
#include <MG_Pipe/MGPipe.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_Remote/Client/ClientSession.h>
#include <MG_Remote/Server/ServerLoop.h>
#include <MG_Remote/Server/ServerSession.h>
#endif

#if MOBILEGL_BUILD_DISAGGREGATED
// [p5/v1-joint PREVIEW EDIT - the merge-time form of v1's hook, c1-v1 8.2's exact order.] The
// weak CreateRemoteBackendObject placeholder v1 shipped for a c1-less tree is DELETED at the
// merge: the integration test links the STATIC MobileGL_s archive, and an archive member is
// only pulled to satisfy an UNDEFINED reference - the weak definition in ServerLoop.cpp.o
// already satisfied it, so BackendObject_Remote.cpp.o was never linked at all and the joint
// build aborted in the placeholder (~/w7/p5-v1-joint-preflight.log). Direct construction
// needs no factory and no weak symbol.
#include <MG_Remote/Client/BackendObject_Remote.h>
#endif

namespace MobileGL::MG_Backend {
    void LogBackendInfo() {
        if (!pActiveBackendObject) {
            MGLOG_W("No active backend object, cannot log backend info");
            return;
        }

        const auto& rendererInfo = pActiveBackendObject->GetRendererInfo();
        MGLOG_I("MobileGL Backend Info:");
        MGLOG_I("  Renderer Name: %s", rendererInfo.RendererName.c_str());
        MGLOG_I("  Backend Name: %s", rendererInfo.BackendName.c_str());
        if (rendererInfo.ExtraVendor) {
            MGLOG_I("  Extra Vendor Info: %s", rendererInfo.ExtraVendor->c_str());
        }
        MGLOG_I("  Target OpenGL Version: %d.%d", rendererInfo.RendererGLInfo.TargetGLVersion.Major,
                rendererInfo.RendererGLInfo.TargetGLVersion.Minor);
        MGLOG_I("  Target GLSL Version: %d.%d", rendererInfo.RendererGLInfo.TargetGLSLVersion.Major,
                rendererInfo.RendererGLInfo.TargetGLSLVersion.Minor);
        MGLOG_I("  OpenGL Extensions:");
        for (const auto& ext : rendererInfo.RendererGLInfo.Extensions) {
            MGLOG_I("    - %s", MG_Util::ConvertGLExtToString(ext).c_str());
        }
    }

    Bool InitSpecificBackendLibs() {
        if (!pActiveBackendObject) {
            MGLOG_W("No active backend object, cannot initialize backend libraries");
            return false;
        }
        pActiveBackendObject->Initialize();
        gBackendFunctionsTable = pActiveBackendObject->GetBackendFunctions();
        return true;
    }

#if MOBILEGL_BUILD_DISAGGREGATED
    namespace {
        // WHICH MGPipe SUBSYSTEMS THIS SERVER HAS A CONSUMER FOR - CallMask bits 32..47 (R-8 /
        // C-4). It is stated from what the server's own backend IS, and NOT derived from
        // MGPipeGetResourceOps(): that is a PROCESS-WIDE global, so under inproc a derivation
        // would answer with whatever the client half of the same process registered and under
        // spawn it would collapse to P2's 0x7f. Either way CapsMirror::ServerConsumes would
        // then answer a client-side liveness gate with a guess, the client would stop emitting
        // whole record families, clear its dirty flags on acceptance anyway, and the lane would
        // go green with the uploads lost - ID-39's 66 lost uploads, reflected (ServerSession.h).
        //
        // DirectGLES consumes all thirteen migrated families (P2's 0..6, P3a's 7..8, P4a's
        // 9..12): it registers the resource op table in Initialize()
        // (BackendObject_DirectGLES.cpp:849) and reads every other family out of gPipeInputs.
        // DirectVulkan registers NO resource ops - MGPipeSetResourceOps has exactly one caller
        // in the whole tree and it is Managers.cpp:2594 - so bit 7 is CLEAR for it, which is
        // the same fact ObjectSubsystemControlScenario already pins from the client side.
        // P5e (MG_Remote/CONTRACT-P5E.md §6.2). THE ONE CONSTANT THE WHOLE PHASE HANGS ON.
        //
        // kCapRunAheadApply says "this server applies an unbarriered record without reading
        // client memory". That is only true once EVERY per-draw family reads records instead
        // of the frontend - vi, sb, pg, tx2 and fb all land before it is - so the caps arm
        // below is gated on this constant, which the P5e INTEGRATION COMMIT flips to true
        // after the last of them. Until then every P5e package lands with the wait rule, the
        // static WaitClass column and the barriered predicate compiled and INERT: the client
        // never latches RunAheadArmed, so it runs today's lockstep path byte for byte.
        //
        // It is a constant and not a knob on purpose. An operator cannot turn a half-migrated
        // server into a run-ahead one, because the failure mode is not a slow frame - it is
        // the apply thread reading client memory that has already moved, which renders wrong
        // rather than aborting.
        constexpr Bool kMGPipeP5eRunAheadReady = true;
        constexpr Bool kMGPipeMagmaRunAheadReady = false;

        // The Magma transport now owns buffer stores and consumes their vertex,
        // index and shader binding records. Both backends publish the resource
        // family only alongside a real op table (checked below). This does not
        // itself establish run-ahead readiness: that has its separate gate below.
        Uint64 ConsumedSubsystemsFor(BackendType type) {
            switch (type) {
            case BackendType::DirectGLES: return MG_Pipe::kMGPipeSubsystemsMigratedAtP5e;
            case BackendType::DirectVulkan:
                return MG_Pipe::kMGPipeSubsystemsMigratedAtP4a | MG_Pipe::kMGPipeSubsystemBufferBindings;
            default: return 0;
            }
        }

        // THE CROSS-CHECK THAT MAKES A WRONG ANSWER LOUD. Claiming bit 7 while no resource op
        // table is registered is the exact failure the mask exists to prevent, one level down:
        // the client would keep emitting the resource family and the server would drop every
        // record of it. The check runs AFTER Initialize(), which is where DirectGLES registers
        // the table, so it can see the real answer rather than a promise.
        void AssertConsumerMaskIsHonest(Uint64 mask) {
            const Bool claimsResources = (mask & MG_Pipe::kMGPipeSubsystemResources) != 0;
            const Bool hasResourceOps = MG_Pipe::MGPipeGetResourceOps() != nullptr;
            if (claimsResources && !hasResourceOps) {
                MGLOG_F("MGPipe: Fatal{ConsumerMaskLie, \"kMGPipeSubsystemResources\"} - the "
                        "server published a consumer bit for the resource family while "
                        "MGPipeGetResourceOps() is null. The client's R-8 liveness gate would "
                        "keep emitting resource_create / resource_subdata records that this "
                        "server drops on the floor, and the client clears its dirty flags on "
                        "acceptance anyway (ID-39). A mask is a statement about this backend, "
                        "not a hope");
                std::abort();
            }
            if (!claimsResources && hasResourceOps) {
                // The safe direction: the legacy pull path keeps running. Said out loud anyway,
                // because it silently costs the whole P3a family its migration.
                MGLOG_W("MG_Remote server: a resource op table is registered but the consumer "
                        "mask withholds kMGPipeSubsystemResources; the buffer family will fall "
                        "back to the legacy path for this session");
            }
        }

        // The resource op table the SERVER's backend registered at step 1 (BackendObject_DirectGLES::
        // Initialize -> RegisterBufferBackendOps), as step 2 saw it. Step 5 compares against it
        // (review v2 N-8): a client object that registered a table of its own would have made
        // AssertConsumerMaskIsHonest's "is a table registered" answer TRUE, so re-asking that
        // question could never notice the swap - only the pointer can.
        const MG_Pipe::MGPipeResourceOps* g_resourceOpsAtStep2 = nullptr;

        // The single hook (ARCHITECTURE.md:29). Returns false when the split could not be
        // brought up, and the caller then REFUSES TO CONTINUE rather than falling back to the
        // switch below - a fallback here is "the split lane ran monolith and went green".
        Bool InitSplitRoles() {
            using namespace MobileGL::MG_Remote;

            // 1. the SERVER role's private backend object, on the app thread, with no GL and no
            //    EGL. The context is created and made current later, on mgl-srv-apply, when the
            //    client's first eglMakeCurrent crosses as a blocking control request.
            Server::ServerLoop& loop = Server::ServerLoopInstance();
            const MobileGLResult created = loop.CreateBackend(MG_Config::ActiveBackendType);
            if (created != MOBILEGL_OK) return false;

            // 2. the two CallMask halves. NEITHER HAS A DEFAULT and CallMask() is a named Fatal
            //    on an unset one (s1's BLOCKER fix), so this is the "somebody" that block names.
            Server::ServerSession& session = Server::ServerSessionInstance();
            const Uint64 consumed = ConsumedSubsystemsFor(MG_Config::ActiveBackendType);
            AssertConsumerMaskIsHonest(consumed);
            g_resourceOpsAtStep2 = MG_Pipe::MGPipeGetResourceOps();
            session.SetConsumedSubsystems(consumed);
            // ZERO IS THE EXPLICIT ANSWER FOR P5, not an omission (ServerSession.h's block):
            // every optional capability bit belongs to the package that owns its question, and
            // withholding one leaves the legacy path running, which is the safe direction.
            // kCapNeedsHostIndexBytes and kCapNeedsHostUboBytes must be 0 for the whole of P5
            // by ruling - they are the only two things that ask for an MGHostSpan, and 0 is
            // what keeps every one of them out of the first IPC frame (contract table 0).
            //
            // P5b t2 (CONTRACT-P5B.md §6.5) PUBLISHES THE ONE BIT P5b ADDS, and this is the
            // only place that can: the question kCapBackendOwnsXfbCapture answers is "does the
            // SERVER's backend own the transform-feedback capture", and the server's table is
            // visible here and nowhere on the client. It is read straight off the table
            // ServerLoop::CreateBackend just built - Espryt registers XfbImpl::EndTransformFeedback
            // (BackendObject_DirectGLES.cpp:1458) and Magma registers no XFB slot at all - so the
            // bit is a statement about THIS backend rather than about a build option, which is
            // what makes it survive a backend switch. The client reads it through
            // MGL_BACKEND_SLOT_CAP at GL_Drawing.cpp's FixupGsStripCaptureOrder.
            Uint64 capBits = 0;
            if (const MG_Backend::BackendObject* serverBackend = loop.Backend();
                serverBackend != nullptr &&
                serverBackend->GetBackendFunctions().GL.EndTransformFeedback != nullptr) {
                capBits |= MG_Pipe::kCapBackendOwnsXfbCapture;
            }
            // Each backend has an independent implementation-readiness gate.
            // The runtime RunAhead knob can decline the feature, never create it.
            const Bool runAheadReady = MG_Config::ActiveBackendType == BackendType::DirectVulkan
                ? kMGPipeMagmaRunAheadReady : kMGPipeP5eRunAheadReady;
            capBits |= MG_Pipe::MGPipeRunAheadCapBitsFor(MG_Config::ActiveBackendType, runAheadReady);
            session.SetCapabilityBits(capBits);
            session.SetBackend(loop.Backend());

            // 3. the handshake, the four segments, and - at its end - the apply thread.
            const MobileGLResult started =
                Client::ClientSessionInstance().Start(MG_Config::Transport, MG_Config::TransportEndpoint);
            if (started != MOBILEGL_OK) {
                MGLOG_E("MG_Remote: the split session failed to start (rc=%d); MobileGL will not "
                        "fall back to monolith - a lane named split that ran monolith is the one "
                        "failure this phase is built to make impossible",
                        static_cast<int>(started));
                return false;
            }

            // 4. and only now the CLIENT's backend object in the one global that holds it.
            //    Table 3: pActiveBackendObject holds BackendObject_Remote and the server's
            //    BackendObject_DirectGLES stays private to ServerLoop.
            pActiveBackendObject = MakeUnique<MG_Remote::Client::BackendObject_Remote>();
            return true;
        }
    } // namespace
#endif

#if MOBILEGL_BUILD_DISAGGREGATED
    void ShutdownSplitRoles() {
        if (MG_Config::Transport == MG_Config::TransportMode::Monolith) return;
        // ClientSession::Stop IS table 3's whole order and it is idempotent: publish and wait
        // for the server to drain (bounded - a lost record must be a red lane, not a hung
        // exit), Doorbell::Kill through the transport's Shutdown, ServerLoop::Stop's bounded
        // join - which also destroys the server's private BackendObject ON the apply thread
        // while it still owns the context - the transport, and only THEN anything an emitter
        // owns. A var-tail still named by an unapplied record is a use-after-free the join is
        // what prevents, which is why the order is not a preference.
        MG_Remote::Client::ClientSessionInstance().Stop();
        // M-6: ClientSession::Stop's !m_started arm (a Start that FAILED after
        // ServerSession::Accept - a refused Accept, an invalid cmd/reply ring) tears down only
        // the client half and never stops the apply thread or drops the server's private backend,
        // which ServerLoop::CreateBackend already built and which holds the process-wide
        // g_resourceOps. So call ServerLoop::Stop() here unconditionally. It is idempotent: on the
        // started path ClientSession::Stop already joined the thread, so this hits Stop's
        // !joinable arm, which resets a backend that never ran a thread and is otherwise a no-op.
        // Without this an early Start failure leaves BackendObject_DirectGLES permanently alive
        // and every later split bring-up in the process fails at CreateBackend's m_backend!=null
        // guard.
        MG_Remote::Server::ServerLoopInstance().Stop();
    }
#endif

    void Init() {
        MGLOG_D("Initializing MobileGL Backend...");

#if MOBILEGL_BUILD_DISAGGREGATED
        // THE SINGLE HOOK. In a build without MOBILEGL_BUILD_DISAGGREGATED, MG_Config::Transport
        // is a `constexpr Monolith` (Config.h) and this whole statement is discarded, so the
        // pull build gains no symbol, no branch and no byte - which is what G1 measures.
        if (MG_Config::Transport != MG_Config::TransportMode::Monolith) {
            if (!InitSplitRoles()) {
                // NOT a fallback to the switch. pActiveBackendObject stays null and the next GL
                // call fails loudly, which is the only honest outcome: the operator asked for a
                // transport this process could not bring up.
                pActiveBackendObject = nullptr;
                return;
            }
            Bool remoteResult = InitSpecificBackendLibs();
            if (!remoteResult) {
                MGLOG_W("Failed to initialize MobileGL backend libraries for the remote object");
                return;
            }
            // m-6, re-worded per review v2 N-8. The honesty cross-check runs a SECOND time, now
            // that step 4's pActiveBackendObject (the client's BackendObject_Remote) exists and
            // its Initialize() has run inside InitSpecificBackendLibs. What the re-run CAN catch
            // is a table that was REMOVED between step 2 and here (the claim would then be a lie
            // again). What it cannot catch - and its first comment claimed it could - is a client
            // object that REGISTERED a table of its own: that leaves "is a table registered"
            // true. Only the pointer tells those apart, so the table is compared against the one
            // step 2 saw and a swap is refused by name: the applier would otherwise dispatch the
            // server's resource records into the CLIENT object's table under its feet.
            AssertConsumerMaskIsHonest(ConsumedSubsystemsFor(MG_Config::ActiveBackendType));
            if (MG_Pipe::MGPipeGetResourceOps() != g_resourceOpsAtStep2) {
                MGLOG_F("MGPipe: Fatal{ConsumerMaskLie, \"resource ops table replaced\"} - the "
                        "resource op table MGPipeGetResourceOps() answers with is not the one the "
                        "server's backend registered at step 1 (%p now, %p then). Something between "
                        "ServerSession::Accept and the client object's Initialize() registered its "
                        "own table, and the applier would dispatch every resource record into it. A "
                        "mask is a statement about the server's backend, and so is the table",
                        static_cast<const void*>(MG_Pipe::MGPipeGetResourceOps()),
                        static_cast<const void*>(g_resourceOpsAtStep2));
                std::abort();
            }
            LogBackendInfo();
            return;
        }
#endif

        switch (MG_Config::ActiveBackendType) {
        case BackendType::DirectGLES:
            pActiveBackendObject = MakeUnique<DirectGLES::BackendObject_DirectGLES>();
            break;
        case BackendType::DirectVulkan:
            pActiveBackendObject = MakeUnique<DirectVulkan::BackendObject_DirectVulkan>();
            break;
        case BackendType::Unknown:
        default:
            MGLOG_W("Unknown backend type, defaulting to unknown backend");
            pActiveBackendObject = nullptr;
        }

        Bool result = InitSpecificBackendLibs();
        if (!result) {
            MGLOG_W("Failed to initialize MobileGL backend libraries");
            return;
        }
        LogBackendInfo();
    }
} // namespace MobileGL::MG_Backend
