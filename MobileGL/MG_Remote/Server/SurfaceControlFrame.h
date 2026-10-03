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
// THE FRAME CARRIES NO POINTERS, and that is enforced by the exhaustive member check below:
// adding a member requires updating its structured binding; pointer types fail the assertion.
// The three places the old Args structs held client addresses map onto values:
//
//   ServerInitializeEGLDisplay's major/minor out-pointers -> the reply fields eglMajor/eglMinor;
//   ServerCreateEGLWindowSurface / ServerSetWindowHandle's const WindowHandle* -> windowBackend +
//     nativeToken + width/height (WindowHandle::Handle as an integer);
//   every EGL handle (display/surface/context) -> a Uint64 token. Inproc the token IS the handle
//     value bit-cast (same process, same driver, and the N-3 tuple bookkeeping compares them);
//     under spawn the client mints dense tokens instead (P6-CONTRACT-DRAFT table 0) - the field
//     width is already the token's, so nothing here changes shape on that day.
//
// PROCESS-LOCAL WINDOW OBJECTS ARE REFUSED BY NAME on the wire: ANativeWindow* and
// CAMetalLayer* mean nothing in the server process. SurfaceOpCodec names their refusals
// AndroidNativeWindow@P12 and MetalLayer@P12, and they stay (Rule H): P12 does not carry a
// client's window across, it gives the SERVER a window of its own (kServerOwnedWindowBackend
// below, WindowKind::ServerOwned on the wire).

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
        // P6 (cp) MOVED THIS ONE ONTO THE WIRE, and the old name was a lie the
        // moment spawn existed. f0-egl reasoned that the wire's ANSWER to
        // InitCapabilities is the CapsSnapshot frame, which is true and beside
        // the point: the REQUEST still has to reach the apply thread, and under
        // spawn that thread is in another process. Measured before the fix -
        // every spawned eglMakeCurrent died at "surface op kind 11 cannot cross
        // the wire (InprocOnlyOpOnTheWire)", one call after the backend came up.
        InitCapabilities = 11,
        // P14 S1 (docs/Disaggregated/design/11-state-ownership.md). The EGL context lifecycle,
        // appended to the schema and to this enum together so the identity cast below stays the
        // whole mapping: the client's eglCreateContext/eglDestroyContext reach the server through
        // THE SAME frame channel the nine surface forwarders use, on the same thread, in the same
        // order - which is the property this slice needs and a second channel would not give.
        CreateContext = 12,
        DestroyContext = 13,
        // INPROC-ONLY kinds: legal inside this process's frame channel, NEVER encodable onto the
        // wire (SurfaceOpCodec refuses them by name). f0-egl's census found no production caller
        // for the two dead forwarders; they ride the same frame channel because the
        // function-pointer mailbox is GONE, not because they are wire ops.
        //
        // THEIR VALUES MOVED 12/13/14 -> 14/15/16 when P14 S1 appended the two context ops. That
        // is safe by construction: these three never cross a process boundary and are named
        // symbolically everywhere they are used, so nothing outside this build could have pinned
        // them - where the eleven above and the two below are wire ABI and may never move.
        SwapBuffersInprocOnly = 14,      // present travels as a record (the class-B Present emitter)
        InitWindowSurfaceInprocOnly = 15,// a client-side no-op; kept for the inproc test lane
        ProbeForTesting = 16,            // MG_Test's arbitrary-work seam through the same channel
    };

    // P12 (on-screen server window), D2. THE SERVER-OWNED WINDOW, AS A FRAME-LOCAL TAG.
    //
    // A client that sets MOBILEGL_IPC_SURFACE=server has no window the server could use (it may
    // have none at all), so its CreateWindowSurface names the SERVER's window instead: the wire's
    // WindowKind::ServerOwned with nativeToken 0. Inside a process the request travels in the
    // frame's `windowBackend` slot as this value - deliberately NOT a MG_Backend::WindowBackend
    // enumerator (BackendObject.h is in the pull build, gate G1), and deliberately outside every
    // range a WindowBackend could grow to, so a frame carrying it that ever reached
    // UnpackWindowHandle would be refused by WindowBackendFromFrameValue's range check rather than
    // cast. The server's ServerOwned arm (ServerLoop.cpp) consumes it before that point and
    // substitutes its own window there; no pointer crosses the wire (Rule G/H).
    inline constexpr Int kServerOwnedWindowBackend = 0x10000;

    // CreatePbufferSurface's flags, in the frame's `contextFlags` word (the wire's SurfaceOp
    // contextFlags, 0 on every op but CreateContext before this, so an older server reads none).
    // SURFACELESS STAND-IN: the hidden 1x1 pbuffer a client binds the backend to for a surfaceless
    // make-current (EGL_KHR_surfaceless_context). It is never presented or read back, so it is no
    // rendering path: it neither latches the session's surface mode nor is refused by it (D4).
    // Without it, a client that made a context current with no surface before it created its
    // server-owned window latched its session offscreen, and the window was refused.
    inline constexpr Uint32 kPbufferFlagSurfacelessStandIn = 1u;

    // P12. Why the server declined a surface op, carried back in the reply half (`refusal`). The
    // values ARE the wire's SurfaceRefusal (protocol.fbs); SurfaceOpCodec pins the agreement with
    // static_asserts, the way it pins SurfaceControlOp against SurfaceOpKind. None of them latches
    // the session.
    enum class SurfaceRefusalCode : Uint8 {
        None = 0,
        NoServerDisplay = 1,              // ServerOwned asked of a server that owns no display
        NoServerWindow = 2,               // a display, but no window within the wait
        SurfaceModeMismatch = 3,          // D4: the session's surface mode is the other one
        ServerOwnedOnSetWindowHandle = 4, // SetWindowHandle named the server's own window
    };

    const char* SurfaceRefusalCodeName(SurfaceRefusalCode code);

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
        // P14 S1. CreateContext's two values beyond `context` (which is the client context token
        // itself): the share group the context was derived into, and the reserved flags word the
        // schema carries so a later attribute has a slot. CreatePbufferSurface carries its own
        // flags in the same word (kPbufferFlagSurfacelessStandIn). 0 on every other op.
        Uint64 shareGroupToken = 0;
        Uint32 contextFlags = 0;
        // The reply half: written by the dispatch on the apply thread, read by the poster after
        // the blocking handshake returns. `ok` is the forwarder's Bool answer; eglMajor/eglMinor
        // are InitializeDisplay's out values.
        Bool ok = false;
        Int eglMajor = 0;
        Int eglMinor = 0;
        Uint64 eventHead = 0; // data-plane delivery fence carried by SurfaceReply
        // P12: a SurfaceRefusalCode (0 = none). `width`/`height` above double as the reply's
        // surface geometry: a ServerOwned CreateWindowSurface answers with the server window's
        // real extent there (SurfaceReply.width/height).
        Uint8 refusal = 0;
    };

    namespace Detail {
        // Pointers are trivially copyable and standard-layout too. Only these value types are
        // allowed; the constexpr negatives pin the distinction from the old, insufficient check.
        template <typename T>
        inline constexpr bool IsSurfaceControlValue = std::is_integral_v<T> || std::is_enum_v<T>;
        static_assert(!IsSurfaceControlValue<void*> && !IsSurfaceControlValue<void (*)()>);

        constexpr bool SurfaceControlFrameHasOnlyValues() {
            SurfaceControlFrame frame{};
            // Exhaustive by the language's aggregate decomposition rule: adding ANY member
            // without extending this binding is a compile error, so new fields cannot evade it.
            const auto& [kind, seq, display, surface, readSurface, context, windowBackend,
                         nativeToken, width, height, swapInterval, shareGroupToken, contextFlags, ok,
                         eglMajor, eglMinor, eventHead, refusal] = frame;
            return IsSurfaceControlValue<decltype(kind)> && IsSurfaceControlValue<decltype(seq)> &&
                   IsSurfaceControlValue<decltype(display)> && IsSurfaceControlValue<decltype(surface)> &&
                   IsSurfaceControlValue<decltype(readSurface)> && IsSurfaceControlValue<decltype(context)> &&
                   IsSurfaceControlValue<decltype(windowBackend)> && IsSurfaceControlValue<decltype(nativeToken)> &&
                   IsSurfaceControlValue<decltype(width)> && IsSurfaceControlValue<decltype(height)> &&
                   IsSurfaceControlValue<decltype(swapInterval)> &&
                   IsSurfaceControlValue<decltype(shareGroupToken)> &&
                   IsSurfaceControlValue<decltype(contextFlags)> && IsSurfaceControlValue<decltype(ok)> &&
                   IsSurfaceControlValue<decltype(eglMajor)> && IsSurfaceControlValue<decltype(eglMinor)> &&
                   IsSurfaceControlValue<decltype(eventHead)> && IsSurfaceControlValue<decltype(refusal)>;
        }
    } // namespace Detail

    static_assert(Detail::SurfaceControlFrameHasOnlyValues(),
                  "every surface control frame member must be an integer or enum, never a pointer");
    static_assert(std::is_trivially_copyable_v<SurfaceControlFrame> &&
                      std::is_standard_layout_v<SurfaceControlFrame>,
                  "the surface control frame must remain a trivially copyable value aggregate");

    const char* SurfaceControlOpName(SurfaceControlOp op);

    // True exactly for the ten ops that have a Wire::SurfaceOpKind. The inproc-only kinds
    // (InitCapabilities/SwapBuffers/InitWindowSurface, and the test probe) answer false: they may
    // ride the inproc frame channel but never the wire.
    bool SurfaceControlOpHasWireKind(SurfaceControlOp op);

} // namespace MobileGL::MG_Remote::Server
