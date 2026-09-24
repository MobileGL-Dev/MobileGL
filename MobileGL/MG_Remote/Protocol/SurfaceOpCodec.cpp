// MobileGL - MobileGL/MG_Remote/Protocol/SurfaceOpCodec.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "SurfaceOpCodec.h"
#include <MG_Remote/FatalFunnel.h>

#include <MG_Backend/BackendObject.h>
#include <MG_Remote/Protocol/generated/protocol_generated.h>
#include <MG_Remote/Server/ServerLoop.h>
#include <MG_Util/Debug/Log.h>

#include <cstdlib>

namespace MobileGL::MG_Remote {

    namespace {

        using Server::SurfaceControlFrame;
        using Server::SurfaceControlOp;

        // THE ALIGNMENT IS PINNED, NOT ASSUMED. The frame's first eleven values were chosen to
        // agree with the wire enum so the tables below read as identity - but a schema edit that
        // renumbers either side fails HERE, at compile time, instead of silently mistranslating
        // op identities between two processes that both still announce the same ABI major.
        static_assert(static_cast<Uint8>(SurfaceControlOp::InitializeDisplay) ==
                          static_cast<Uint8>(::MobileGL::Wire::SurfaceOpKind::InitializeDisplay));
        static_assert(static_cast<Uint8>(SurfaceControlOp::CreateWindowSurface) ==
                          static_cast<Uint8>(::MobileGL::Wire::SurfaceOpKind::CreateWindowSurface));
        static_assert(static_cast<Uint8>(SurfaceControlOp::CreatePbufferSurface) ==
                          static_cast<Uint8>(::MobileGL::Wire::SurfaceOpKind::CreatePbufferSurface));
        static_assert(static_cast<Uint8>(SurfaceControlOp::ResizeWindowSurface) ==
                          static_cast<Uint8>(::MobileGL::Wire::SurfaceOpKind::ResizeWindowSurface));
        static_assert(static_cast<Uint8>(SurfaceControlOp::ReleaseSurface) ==
                          static_cast<Uint8>(::MobileGL::Wire::SurfaceOpKind::ReleaseSurface));
        static_assert(static_cast<Uint8>(SurfaceControlOp::MakeCurrent) ==
                          static_cast<Uint8>(::MobileGL::Wire::SurfaceOpKind::MakeCurrent));
        static_assert(static_cast<Uint8>(SurfaceControlOp::ReleaseCurrent) ==
                          static_cast<Uint8>(::MobileGL::Wire::SurfaceOpKind::ReleaseCurrent));
        // fc's three schema additions, append-only (protocol.fbs's own rule).
        static_assert(static_cast<Uint8>(SurfaceControlOp::SetSwapInterval) ==
                          static_cast<Uint8>(::MobileGL::Wire::SurfaceOpKind::SetSwapInterval));
        static_assert(static_cast<Uint8>(SurfaceControlOp::ReleaseResources) ==
                          static_cast<Uint8>(::MobileGL::Wire::SurfaceOpKind::ReleaseResources));
        static_assert(static_cast<Uint8>(SurfaceControlOp::SetWindowHandle) ==
                          static_cast<Uint8>(::MobileGL::Wire::SurfaceOpKind::SetWindowHandle));
        // cp's one schema addition, on the same append-only terms.
        static_assert(static_cast<Uint8>(SurfaceControlOp::InitCapabilities) ==
                          static_cast<Uint8>(::MobileGL::Wire::SurfaceOpKind::InitCapabilities));
        static_assert(static_cast<Uint8>(::MobileGL::Wire::SurfaceOpKind::SetSwapInterval) == 8 &&
                      static_cast<Uint8>(::MobileGL::Wire::SurfaceOpKind::ReleaseResources) == 9 &&
                      static_cast<Uint8>(::MobileGL::Wire::SurfaceOpKind::SetWindowHandle) == 10 &&
                      static_cast<Uint8>(::MobileGL::Wire::SurfaceOpKind::InitCapabilities) == 11);
        static_assert(static_cast<Uint8>(::MobileGL::Wire::WindowKind::MetalLayer) == 6);
        // P12's append, on the same terms: the value is wire ABI.
        static_assert(static_cast<Uint8>(::MobileGL::Wire::WindowKind::ServerOwned) == 7);
        // The frame's refusal codes ARE the wire's SurfaceRefusal (SurfaceControlFrame.h), pinned
        // value by value so a schema edit that renumbers either side fails here.
        using Server::SurfaceRefusalCode;
        static_assert(static_cast<Uint8>(SurfaceRefusalCode::None) ==
                      static_cast<Uint8>(::MobileGL::Wire::SurfaceRefusal::None));
        static_assert(static_cast<Uint8>(SurfaceRefusalCode::NoServerDisplay) ==
                      static_cast<Uint8>(::MobileGL::Wire::SurfaceRefusal::NoServerDisplay));
        static_assert(static_cast<Uint8>(SurfaceRefusalCode::NoServerWindow) ==
                      static_cast<Uint8>(::MobileGL::Wire::SurfaceRefusal::NoServerWindow));
        static_assert(static_cast<Uint8>(SurfaceRefusalCode::SurfaceModeMismatch) ==
                      static_cast<Uint8>(::MobileGL::Wire::SurfaceRefusal::SurfaceModeMismatch));
        static_assert(static_cast<Uint8>(SurfaceRefusalCode::ServerOwnedOnSetWindowHandle) ==
                      static_cast<Uint8>(::MobileGL::Wire::SurfaceRefusal::ServerOwnedOnSetWindowHandle));
        // The frame-local ServerOwned tag names no WindowBackend, by construction (D2: no
        // BackendObject.h enumerator for it - that header is in the pull build, gate G1).
        static_assert(Server::kServerOwnedWindowBackend >=
                          static_cast<Int>(MG_Backend::WindowBackend::WindowBackendCount),
                      "kServerOwnedWindowBackend collides with a real WindowBackend");

        bool OpNeedsAWindowBackend(SurfaceControlOp op) {
            return op == SurfaceControlOp::CreateWindowSurface || op == SurfaceControlOp::SetWindowHandle;
        }

    } // namespace

    const char* SurfaceWireErrorName(SurfaceWireError error) {
        switch (error) {
        case SurfaceWireError::None: return "None";
        case SurfaceWireError::UnknownOpKind: return "UnknownOpKind";
        case SurfaceWireError::InprocOnlyOpOnTheWire: return "InprocOnlyOpOnTheWire";
        case SurfaceWireError::WindowKindNamesNoBackend: return "WindowKindNamesNoBackend";
        case SurfaceWireError::UnknownWindowKind: return "UnknownWindowKind";
        case SurfaceWireError::MetalLayerArrived: return "MetalLayerArrived";
        case SurfaceWireError::AndroidNativeWindowArrived: return "AndroidNativeWindowArrived";
        case SurfaceWireError::ServerOwnedTokenNotZero: return "ServerOwnedTokenNotZero";
        case SurfaceWireError::ServerOwnedOnSetWindowHandle: return "ServerOwnedOnSetWindowHandle";
        }
        return "<unknown SurfaceWireError>";
    }

    bool WireKindForSurfaceControlOp(SurfaceControlOp op, ::MobileGL::Wire::SurfaceOpKind* out) {
        if (!Server::SurfaceControlOpHasWireKind(op)) return false;
        // The static_asserts above make this cast a table lookup spelled as a conversion: every
        // kind that reaches this line has a pinned, equal wire value.
        *out = static_cast<::MobileGL::Wire::SurfaceOpKind>(static_cast<Uint8>(op));
        return true;
    }

    bool SurfaceControlOpForWireKind(::MobileGL::Wire::SurfaceOpKind kind, SurfaceControlOp* out) {
        // THE UPPER BOUND IS THE LAST APPENDED KIND, and it moves with every
        // append or the new tag decodes as out-of-range - which reads as
        // Fatal{ProtocolCorruption} at ServerApplyWireSurfaceOp rather than as
        // the missing row it is.
        if (::flatbuffers::IsOutRange(kind, ::MobileGL::Wire::SurfaceOpKind::InitializeDisplay,
                                      ::MobileGL::Wire::SurfaceOpKind::InitCapabilities)) {
            return false;
        }
        const SurfaceControlOp op = static_cast<SurfaceControlOp>(static_cast<Uint8>(kind));
        if (!Server::SurfaceControlOpHasWireKind(op)) return false;
        *out = op;
        return true;
    }

    bool WireWindowKindForWindowBackend(MG_Backend::WindowBackend backend, ::MobileGL::Wire::WindowKind* out) {
        switch (backend) {
        case MG_Backend::WindowBackend::Android: *out = ::MobileGL::Wire::WindowKind::AndroidNativeWindow; return true;
        case MG_Backend::WindowBackend::X11: *out = ::MobileGL::Wire::WindowKind::X11; return true;
        case MG_Backend::WindowBackend::MetalLayer: *out = ::MobileGL::Wire::WindowKind::MetalLayer; return true;
        case MG_Backend::WindowBackend::Win32: *out = ::MobileGL::Wire::WindowKind::Win32Hwnd; return true;
        case MG_Backend::WindowBackend::Unknown: *out = ::MobileGL::Wire::WindowKind::None; return true;
        default: return false;
        }
    }

    bool WindowBackendForWireWindowKind(::MobileGL::Wire::WindowKind kind, MG_Backend::WindowBackend* out) {
        switch (kind) {
        case ::MobileGL::Wire::WindowKind::AndroidNativeWindow: *out = MG_Backend::WindowBackend::Android; return true;
        case ::MobileGL::Wire::WindowKind::X11: *out = MG_Backend::WindowBackend::X11; return true;
        case ::MobileGL::Wire::WindowKind::MetalLayer: *out = MG_Backend::WindowBackend::MetalLayer; return true;
        case ::MobileGL::Wire::WindowKind::Win32Hwnd: *out = MG_Backend::WindowBackend::Win32; return true;
        case ::MobileGL::Wire::WindowKind::None: *out = MG_Backend::WindowBackend::Unknown; return true;
            // Surfaceless and Pbuffer are surface SHAPES, not window backends: no answer.
        default: return false;
        }
    }

    SurfaceWireError EncodeSurfaceOpFrame(const SurfaceControlFrame& frame,
                                          flatbuffers::FlatBufferBuilder* builder) {
        ::MobileGL::Wire::SurfaceOpKind kind;
        if (!WireKindForSurfaceControlOp(frame.kind, &kind)) {
            return frame.kind == SurfaceControlOp::None ? SurfaceWireError::UnknownOpKind
                                                        : SurfaceWireError::InprocOnlyOpOnTheWire;
        }
        ::MobileGL::Wire::WindowKind windowKind = ::MobileGL::Wire::WindowKind::None;
        Uint64 nativeToken = frame.nativeToken;
        if (frame.kind == SurfaceControlOp::CreatePbufferSurface) {
            // The op implies the shape; the frame carries no windowBackend for it.
            windowKind = ::MobileGL::Wire::WindowKind::Pbuffer;
        } else if (OpNeedsAWindowBackend(frame.kind)) {
            if (frame.windowBackend == Server::kServerOwnedWindowBackend) {
                // P12 (D2): the SERVER's window. Only a creation may ask for it - the server picks
                // the window, so there is nothing for SetWindowHandle to set - and the token is 0
                // whatever the frame carried: the client's own window value (it may have none)
                // never reaches the wire (Rule G/H).
                if (frame.kind != SurfaceControlOp::CreateWindowSurface) {
                    return SurfaceWireError::ServerOwnedOnSetWindowHandle;
                }
                windowKind = ::MobileGL::Wire::WindowKind::ServerOwned;
                nativeToken = 0;
            } else if (!WireWindowKindForWindowBackend(
                           static_cast<MG_Backend::WindowBackend>(frame.windowBackend), &windowKind)) {
                return SurfaceWireError::UnknownWindowKind;
            }
        }
        const auto op = ::MobileGL::Wire::CreateSurfaceOp(
            *builder, frame.seq, kind, frame.display, frame.surface, windowKind, nativeToken,
            frame.width, frame.height, frame.swapInterval, frame.readSurface, frame.context);
        const auto envelope = ::MobileGL::Wire::CreateCtrlEnvelope(*builder, ::MobileGL::Wire::CtrlMsg::SurfaceOp, op.Union());
        ::MobileGL::Wire::FinishCtrlEnvelopeBuffer(*builder, envelope);
        return SurfaceWireError::None;
    }

    SurfaceWireError DecodeWireSurfaceOp(const ::MobileGL::Wire::SurfaceOp& op, SurfaceControlFrame* frame) {
        SurfaceControlOp kind;
        if (!SurfaceControlOpForWireKind(op.kind(), &kind)) {
            return SurfaceWireError::UnknownOpKind;
        }
        SurfaceControlFrame decoded;
        decoded.kind = kind;
        decoded.seq = op.seq();
        decoded.display = op.display();
        decoded.surface = op.surface();
        decoded.readSurface = op.readSurface();
        decoded.context = op.context();
        decoded.width = op.width();
        decoded.height = op.height();
        decoded.swapInterval = op.swapInterval();
        if (OpNeedsAWindowBackend(kind)) {
            if (op.windowKind() == ::MobileGL::Wire::WindowKind::ServerOwned) {
                // P12 (D2). Asked BEFORE the Android refusal below, which it is not: the window is
                // the server's own. A creation only (SetWindowHandle may not name it - a named
                // refusal, not corruption: nothing about the bytes is malformed), and a token of 0
                // only (anything else is a value from the peer's window system trying to cross,
                // Rule G - that one is corruption).
                if (kind != SurfaceControlOp::CreateWindowSurface) {
                    return SurfaceWireError::ServerOwnedOnSetWindowHandle;
                }
                if (op.nativeToken() != 0) {
                    return SurfaceWireError::ServerOwnedTokenNotZero;
                }
                decoded.windowBackend = Server::kServerOwnedWindowBackend;
                decoded.nativeToken = 0;
                *frame = decoded;
                return SurfaceWireError::None;
            }
            if (op.windowKind() == ::MobileGL::Wire::WindowKind::AndroidNativeWindow) {
                // The one refusal that is not corruption: the window kind is LEGAL and the token
                // is an ANativeWindow*, which names memory in the CLIENT's process. Real window
                // arrival is P12; until then this is refused by name at ServerApplyWireSurfaceOp.
                return SurfaceWireError::AndroidNativeWindowArrived;
            }
            if (op.windowKind() == ::MobileGL::Wire::WindowKind::MetalLayer) {
                // CAMetalLayer* is a client-process object address, just like ANativeWindow*.
                return SurfaceWireError::MetalLayerArrived;
            }
            MG_Backend::WindowBackend backend;
            if (!WindowBackendForWireWindowKind(op.windowKind(), &backend)) {
                // THE UPPER BOUND IS THE LAST APPENDED KIND (P12: ServerOwned), for the reason
                // SurfaceControlOpForWireKind's bound gives.
                return ::flatbuffers::IsOutRange(op.windowKind(), ::MobileGL::Wire::WindowKind::None,
                                                 ::MobileGL::Wire::WindowKind::ServerOwned)
                           ? SurfaceWireError::UnknownWindowKind
                           : SurfaceWireError::WindowKindNamesNoBackend;
            }
            decoded.windowBackend = static_cast<Int>(backend);
            decoded.nativeToken = op.nativeToken();
        }
        *frame = decoded;
        return SurfaceWireError::None;
    }

    void EncodeSurfaceReplyFrame(const SurfaceControlFrame& frame,
                                 flatbuffers::FlatBufferBuilder* builder) {
        // P12: the reply's surface geometry and refusal (SurfaceReply.width/height/refusal).
        const auto reply = ::MobileGL::Wire::CreateSurfaceReply(
            *builder, frame.seq, frame.ok, frame.eglMajor, frame.eglMinor, /*defaultFb=*/0, frame.eventHead,
            frame.width, frame.height, static_cast<::MobileGL::Wire::SurfaceRefusal>(frame.refusal));
        const auto envelope =
            ::MobileGL::Wire::CreateCtrlEnvelope(*builder, ::MobileGL::Wire::CtrlMsg::SurfaceReply, reply.Union());
        ::MobileGL::Wire::FinishCtrlEnvelopeBuffer(*builder, envelope);
    }

    void DecodeWireSurfaceReply(const ::MobileGL::Wire::SurfaceReply& reply, SurfaceControlFrame* frame) {
        frame->seq = reply.seq();
        frame->ok = reply.ok();
        frame->eglMajor = reply.eglMajor();
        frame->eglMinor = reply.eglMinor();
        frame->eventHead = reply.eventHead();
        frame->width = reply.width();
        frame->height = reply.height();
        frame->refusal = static_cast<Uint8>(reply.refusal());
    }

    MobileGLResult ServerApplyWireSurfaceOp(const ::MobileGL::Wire::SurfaceOp& op, SurfaceControlFrame* replyOut) {
        // PH-1 (3), ID-P7-1: a latched session answers no further control op. RunSession stops
        // reading the control connection once it sees the latch; this covers the op it may
        // already be holding.
        if (SessionLatched()) return MOBILEGL_ERR_PROTOCOL_MISMATCH;
        SurfaceControlFrame frame;
        const SurfaceWireError error = DecodeWireSurfaceOp(op, &frame);
        if (error == SurfaceWireError::ServerOwnedOnSetWindowHandle) {
            // P12 (D2): A NAMED REFUSAL, NOT A LATCH. Nothing in the bytes is malformed - the op
            // simply asks for something the protocol does not do (the server chooses its own
            // window at CreateWindowSurface; there is nothing for SetWindowHandle to set). So it
            // is answered ok=false, by name, and the session goes on, armed or not.
            MGLOG_E("MG_Remote server: Refuse ServerOwned on SetWindowHandle - a wire SetWindowHandle "
                    "(seq %llu) named WindowKind::ServerOwned. The server-owned window is chosen by the "
                    "server when CreateWindowSurface asks for it; SetWindowHandle may not name it "
                    "(refusal ServerOwnedOnSetWindowHandle, session not latched)",
                    static_cast<unsigned long long>(op.seq()));
            if (replyOut != nullptr) {
                *replyOut = SurfaceControlFrame{};
                replyOut->seq = op.seq();
                replyOut->ok = false;
                replyOut->refusal = static_cast<Uint8>(Server::SurfaceRefusalCode::ServerOwnedOnSetWindowHandle);
            }
            return MOBILEGL_ERR_UNSUPPORTED;
        }
        if (error != SurfaceWireError::None) {
            // PH-1 (3): the op's bytes are the peer's. Each of the three refusals latches in an
            // armed session child and the op is answered with no dispatch; unarmed they die.
            if (error == SurfaceWireError::ServerOwnedTokenNotZero) {
                // P12 (D2): WindowKind::ServerOwned names the SERVER's window, so its token is 0 by
                // definition. A non-zero one is a value from the peer's window system trying to
                // cross (Rule G) - the peer's corrupt bytes, latched like the rows beside it.
                (void)SessionLatch(MGFatalFamily::ProtocolCorruption,
                        "MGPipe: Fatal{ProtocolCorruption, \"SurfaceOp.nativeToken\"} - a wire %s named "
                        "WindowKind::ServerOwned with native token 0x%llx. The server-owned window is the "
                        "server's own and its token is always 0: no value from the client's window system "
                        "crosses (Rule G)",
                        ::MobileGL::Wire::EnumNameSurfaceOpKind(op.kind()),
                        static_cast<unsigned long long>(op.nativeToken()));
            } else if (error == SurfaceWireError::AndroidNativeWindowArrived) {
                (void)SessionLatch(MGFatalFamily::UnmigratedSurface,
                        "MGPipe: Fatal{UnmigratedSurface, \"AndroidNativeWindow@P12\"} - a wire "
                        "SurfaceOp (%s) named an ANativeWindow*, which is a pointer into the "
                        "CLIENT's process and means nothing here. Real window arrival is P12; "
                        "until then the spawn surface path is pbuffer/surfaceless only",
                        ::MobileGL::Wire::EnumNameSurfaceOpKind(op.kind()));
            } else if (error == SurfaceWireError::MetalLayerArrived) {
                (void)SessionLatch(MGFatalFamily::UnmigratedSurface,
                        "MGPipe: Fatal{UnmigratedSurface, \"MetalLayer@P12\"} - a wire "
                        "SurfaceOp named a CAMetalLayer* in the CLIENT's process. "
                        "Real window arrival is P12");
            } else {
                (void)SessionLatch(MGFatalFamily::ProtocolCorruption,
                        "MGPipe: Fatal{ProtocolCorruption, \"SurfaceOp\"} - a wire surface op "
                        "failed validation: %s (wire kind %u, window kind %u)",
                        SurfaceWireErrorName(error), static_cast<unsigned>(op.kind()),
                        static_cast<unsigned>(op.windowKind()));
            }
            if (replyOut != nullptr) {
                *replyOut = SurfaceControlFrame{};
                replyOut->seq = op.seq();
                replyOut->ok = false;
            }
            return MOBILEGL_ERR_PROTOCOL_MISMATCH;
        }
        const MobileGLResult rc = Server::ServerLoopInstance().RunSurfaceControlFrame(frame);
        if (replyOut != nullptr) *replyOut = frame;
        return rc;
    }

} // namespace MobileGL::MG_Remote
