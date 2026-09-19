// MobileGL - MobileGL/MG_Remote/Server/SurfaceControlFrame.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P5f, package fc: the EGL/surface control plane, framed.
//
// WHAT THIS REPLACES. The twelve Server* forwarders used to cross to the apply thread through a
// one-slot mailbox carrying a raw FUNCTION POINTER plus a void* to a stack-local Args struct
// (P5c audit row G4). Both halves are meaningless across a process boundary, which is the whole
// reason P5f exists. The frame below is the seam f0-egl's census prescribed: every forwarder's
// arguments converge into one pure-value struct; inproc the struct crosses the (retained,
// blocking, one-slot) channel by value; under spawn the SAME struct is what SurfaceOpCodec
// encodes into a Wire::SurfaceOp. The thread model does not move - only the payload's shape.
//
// THE FRAME CARRIES NO POINTERS, and that is enforced by construction (the static_assert below)
// rather than by review: a field that is a pointer would also be a compile error here. The three
// places the old Args structs held client addresses map onto values:
//
//   ServerInitializeEGLDisplay's major/minor out-pointers -> the reply fields eglMajor/eglMinor;
//   ServerCreateEGLWindowSurface / ServerSetWindowHandle's const WindowHandle* -> windowBackend +
//     nativeToken + width/height (WindowHandle::Handle as an integer);
//   every EGL handle (display/surface/context) -> a Uint64 token. Inproc the token IS the handle
//     value bit-cast (same process, same driver, and the N-3 tuple bookkeeping compares them);
//     under spawn the client mints dense tokens instead (P6-CONTRACT-DRAFT table 0) - the field
//     width is already the token's, so nothing here changes shape on that day.
//
// AN ANativeWindow* ON THE WIRE IS REFUSED BY NAME, not carried: nativeToken is only meaningful
// inside the client's process for AndroidNativeWindow, and a spawn-side decode of that kind dies
// with Fatal{UnmigratedSurface, "AndroidNativeWindow@P12"} (SurfaceOpCodec). Real window arrival
// is P12.

#pragma once
#include <Includes.h>

#include <type_traits>

namespace MobileGL::MG_Remote::Server {

    // The operation identity - the one piece of information the retired trampoline function
    // pointer carried. The first eleven values are aligned with Wire::SurfaceOpKind
    // (protocol.fbs) ON PURPOSE and SurfaceOpCodec pins that agreement with static_asserts; the
    // mapping is still an explicit table, never a cast.
    enum class SurfaceControlOp : Uint8 {
        None = 0,
        InitializeDisplay = 1,
        CreateWindowSurface = 2,
        CreatePbufferSurface = 3,
        ResizeWindowSurface = 4,
        ReleaseSurface = 5,
        MakeCurrent = 6,
        ReleaseCurrent = 7,
        SetSwapInterval = 8,
        ReleaseResources = 9,
        SetWindowHandle = 10,
        // INPROC-ONLY kinds: legal inside this process's frame channel, NEVER encodable onto the
        // wire (SurfaceOpCodec refuses them by name). f0-egl's census found no production caller
        // for the two dead forwarders and a CapsSnapshot answer for the third; they ride the same
        // frame channel because the function-pointer mailbox is GONE, not because they are wire
        // ops.
        InitCapabilitiesInprocOnly = 11, // the wire's answer is the CapsSnapshot frame itself
        SwapBuffersInprocOnly = 12,      // present travels as a record (the class-B Present emitter)
        InitWindowSurfaceInprocOnly = 13,// a client-side no-op; kept for the inproc test lane
        ProbeForTesting = 14,            // MG_Test's arbitrary-work seam through the same channel
    };

    struct SurfaceControlFrame {
        SurfaceControlOp kind = SurfaceControlOp::None;
        // Minted by the poster (RunSurfaceControlFrame), echoed back with the reply. 0 means
        // "never posted". Purely diagnostic inproc; under spawn it is how a SurfaceReply names
        // its op.
        Uint64 seq = 0;
        Uint64 display = 0;
        Uint64 surface = 0;     // the DRAW surface for MakeCurrent
        Uint64 readSurface = 0; // MakeCurrent only; 0 elsewhere
        Uint64 context = 0;     // MakeCurrent / ReleaseCurrent; 0 elsewhere
        // WindowHandle's payload, as values. This is the BACKEND tag (MG_Backend::WindowBackend
        // as an Int, -1 = Unknown), not the wire's WindowKind: the apply-side dispatch needs the
        // backend enum, and the WindowKind mapping belongs to the wire codec, not to the frame.
        Int windowBackend = -1;
        Uint64 nativeToken = 0; // HWND / XID / ANativeWindow* as an integer
        Int width = 0;
        Int height = 0;
        Int swapInterval = 0;
        // The reply half: written by the dispatch on the apply thread, read by the poster after
        // the blocking handshake returns. `ok` is the forwarder's Bool answer; eglMajor/eglMinor
        // are InitializeDisplay's out values.
        Bool ok = false;
        Int eglMajor = 0;
        Int eglMinor = 0;
    };

    // THE INVARIANT THE PACKAGE EXISTS FOR. The frame crosses by VALUE - copied into the one-slot
    // channel inproc, encoded into a FlatBuffers table under spawn - so it can never carry an
    // address: no pointer members, no ownership, trivially copyable, standard layout.
    static_assert(std::is_trivially_copyable_v<SurfaceControlFrame> &&
                      std::is_standard_layout_v<SurfaceControlFrame>,
                  "the surface control frame must stay a plain bag of scalars - a pointer member "
                  "would be a client address crossing to the server, which is what P5f removes");

    const char* SurfaceControlOpName(SurfaceControlOp op);

    // True exactly for the ten ops that have a Wire::SurfaceOpKind. The inproc-only kinds
    // (InitCapabilities/SwapBuffers/InitWindowSurface, and the test probe) answer false: they may
    // ride the inproc frame channel but never the wire.
    bool SurfaceControlOpHasWireKind(SurfaceControlOp op);

} // namespace MobileGL::MG_Remote::Server
