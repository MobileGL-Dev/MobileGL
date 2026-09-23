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
        if (frame.kind == SurfaceControlOp::CreatePbufferSurface) {
            // The op implies the shape; the frame carries no windowBackend for it.
            windowKind = ::MobileGL::Wire::WindowKind::Pbuffer;
        } else if (OpNeedsAWindowBackend(frame.kind)) {
            if (!WireWindowKindForWindowBackend(
                    static_cast<MG_Backend::WindowBackend>(frame.windowBackend), &windowKind)) {
                return SurfaceWireError::UnknownWindowKind;
            }
        }
        const auto op = ::MobileGL::Wire::CreateSurfaceOp(
            *builder, frame.seq, kind, frame.display, frame.surface, windowKind, frame.nativeToken,
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
                return ::flatbuffers::IsOutRange(op.windowKind(), ::MobileGL::Wire::WindowKind::None,
                                                 ::MobileGL::Wire::WindowKind::MetalLayer)
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
        const auto reply = ::MobileGL::Wire::CreateSurfaceReply(*builder, frame.seq, frame.ok, frame.eglMajor,
                                                    frame.eglMinor, /*defaultFb=*/0, frame.eventHead);
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
    }

    MobileGLResult ServerApplyWireSurfaceOp(const ::MobileGL::Wire::SurfaceOp& op, SurfaceControlFrame* replyOut) {
        // PH-1 (3), ID-P7-1: a latched session answers no further control op. RunSession stops
        // reading the control connection once it sees the latch; this covers the op it may
        // already be holding.
        if (SessionLatched()) return MOBILEGL_ERR_PROTOCOL_MISMATCH;
        SurfaceControlFrame frame;
        const SurfaceWireError error = DecodeWireSurfaceOp(op, &frame);
        if (error != SurfaceWireError::None) {
            // PH-1 (3): the op's bytes are the peer's. Each of the three refusals latches in an
            // armed session child and the op is answered with no dispatch; unarmed they die.
            if (error == SurfaceWireError::AndroidNativeWindowArrived) {
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
