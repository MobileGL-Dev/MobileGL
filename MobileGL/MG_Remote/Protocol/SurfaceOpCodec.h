// MobileGL - MobileGL/MG_Remote/Protocol/SurfaceOpCodec.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P5f, package fc: the codec between the inproc SurfaceControlFrame and the wire's
// Wire::SurfaceOp / Wire::SurfaceReply (protocol.fbs).
//
// The frame is the inproc payload (Server/SurfaceControlFrame.h); this is the ONLY place the
// frame meets the schema. Nothing in the inproc hot path encodes - the frame crosses the one-slot
// channel by value - but the codec is what makes "the same struct encodes under spawn" a tested
// claim rather than a plan, and it owns the two schema seams that are NOT integer translations:
//
//   * SurfaceControlOp <-> Wire::SurfaceOpKind: numerically aligned for the ten framed ops and
//     PINNED by static_asserts in the .cpp, but mapped through an explicit table. The four
//     inproc-only kinds have no wire kind and refuse to encode by name.
//   * MG_Backend::WindowBackend <-> Wire::WindowKind: the two enums do not agree (WindowKind's
//     Surfaceless/Pbuffer are surface shapes, not window backends; WindowBackend's MetalLayer had
//     no wire enumerator until fc added it), so the mapping is a table with named failures, never
//     a cast (f0-egl census, schema gap 3).
//
// ERROR SEMANTICS. Decode is pure: it answers a SurfaceWireError. The FATALS live at the server
// entry point, ServerApplyWireSurfaceOp, because "a wire op the server cannot honour" is a
// contract violation, not a data problem: AndroidNativeWindowArrived dies
// Fatal{UnmigratedSurface, "AndroidNativeWindow@P12"} (the ANativeWindow* means nothing in the
// server's process; real window arrival is P12) and everything else dies
// Fatal{ProtocolCorruption, "SurfaceOp"}. P6's control pump is the intended caller; P5f's unit
// tests drive it directly.

#pragma once
#include <Includes.h>

// The generated schema header (not forward declarations): flatbuffers 25 makes
// FlatBufferBuilder an alias template instantiation, which cannot be opaque-declared, and the
// Wire enums' underlying types are the schema's own - so this header is the dependency, kept
// honest by being the ONLY schema include the codec needs.
#include <MG_Remote/Protocol/generated/protocol_generated.h>
#include <MG_Remote/Protocol/mg_protocol_base.h>
#include <MG_Remote/Server/SurfaceControlFrame.h>

namespace MobileGL::MG_Backend {
    enum class WindowBackend; // BackendObject.h; opaque-declared so this header stays light
} // namespace MobileGL::MG_Backend

namespace MobileGL::MG_Remote {

    enum class SurfaceWireError {
        None = 0,
        UnknownOpKind,           // no SurfaceControlOp for the wire kind (or None on the wire)
        InprocOnlyOpOnTheWire,   // a kind that may ride the inproc channel but never the wire
        WindowKindNamesNoBackend,// Surfaceless/Pbuffer where a window backend is required
        UnknownWindowKind,       // a WindowKind the mapping table does not know
        AndroidNativeWindowArrived, // -> Fatal{UnmigratedSurface, "AndroidNativeWindow@P12"}
    };

    const char* SurfaceWireErrorName(SurfaceWireError error);

    // frame.kind <-> wire kind. Both answer false (and leave *out untouched) for None, the four
    // inproc-only kinds, and any wire value the schema does not define.
    bool WireKindForSurfaceControlOp(Server::SurfaceControlOp op, Wire::SurfaceOpKind* out);
    bool SurfaceControlOpForWireKind(Wire::SurfaceOpKind kind, Server::SurfaceControlOp* out);

    // The explicit WindowBackend <-> WindowKind table. WireWindowKindForWindowBackend also maps
    // WindowBackend::Unknown to WindowKind::None; WindowBackendForWireWindowKind maps
    // WindowKind::None back to Unknown and answers FALSE for Surfaceless/Pbuffer (surface shapes,
    // not window backends).
    bool WireWindowKindForWindowBackend(MG_Backend::WindowBackend backend, Wire::WindowKind* out);
    bool WindowBackendForWireWindowKind(Wire::WindowKind kind, MG_Backend::WindowBackend* out);

    // Encode one frame as a complete CtrlEnvelope{SurfaceOp} buffer (the builder is left
    // Finished). Fails BEFORE touching the builder for a kind that may not cross.
    SurfaceWireError EncodeSurfaceOpFrame(const Server::SurfaceControlFrame& frame,
                                          flatbuffers::FlatBufferBuilder* builder);

    // Validate + decode a wire SurfaceOp into a frame. Pure - it never aborts; the Fatal
    // decisions belong to ServerApplyWireSurfaceOp. The reply half of the frame is reset.
    SurfaceWireError DecodeWireSurfaceOp(const Wire::SurfaceOp& op, Server::SurfaceControlFrame* frame);

    // The reply direction, same shape (CtrlEnvelope{SurfaceReply}).
    void EncodeSurfaceReplyFrame(const Server::SurfaceControlFrame& frame,
                                 flatbuffers::FlatBufferBuilder* builder);
    void DecodeWireSurfaceReply(const Wire::SurfaceReply& reply, Server::SurfaceControlFrame* frame);

    // The server-side wire entry: decode, then post the frame through ServerLoop's channel and
    // hand the reply half back. A decode error is a NAMED Fatal (see the header block), never a
    // dropped message. No transport calls this yet - P6's control pump will.
    MobileGLResult ServerApplyWireSurfaceOp(const Wire::SurfaceOp& op,
                                            Server::SurfaceControlFrame* replyOut);

} // namespace MobileGL::MG_Remote
