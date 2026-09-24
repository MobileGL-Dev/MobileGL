// MobileGL - MobileGL/MG_Remote/Server/ServerDisplay.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P12 (on-screen server window), D3/D6. THE SERVER'S OWN WINDOW, AS ONE PROCESS-WIDE HOLDER.
//
// WHAT IT IS FOR. A client that sets MOBILEGL_IPC_SURFACE=server is headless: it asks the server to
// create its window surface on the SERVER's window (WindowKind::ServerOwned) and learns the
// surface's geometry back from the server. That window belongs to the in-process display server -
// on Android the display Activity's SurfaceView, handed in through JNI (the next stage's glue) -
// and this class is where it is held between the UI thread that owns the window's lifecycle
// (surfaceCreated / surfaceChanged / surfaceDestroyed) and the apply thread that renders into it.
//
// WHO HAS A DISPLAY. Nobody, until Install(): the Linux host server, the exec'd supervisor and
// every session child it forks never call it, so HasDisplay() is false there and a ServerOwned
// request is refused by name (ServerLoop.cpp's ServerOwned arm, SurfaceRefusal::NoServerDisplay).
// Only the in-process display server installs one.
//
// THE WINDOW IS OPAQUE HERE. It is a `void*` with three hooks - acquire and release (Android:
// ANativeWindow_acquire / ANativeWindow_release) and a geometry request (the native -> Java
// up-call: SurfaceHolder.setFixedSize(w, h) for w, h > 0, setSizeFromLayout() for 0/0). That is
// what lets the host unit tests drive attach / wait / detach with a fake handle, and it keeps this
// file free of any platform header.
//
// THE LEASE. The apply thread that substitutes the window into a backend takes a LEASE on it
// (AcquireFor) and names a hook that asks it to let go. Detach() - the surfaceDestroyed path -
// calls that hook and then BLOCKS, bounded, until the holder has released its backend surface and
// ended the lease (EndLease); only then is the window's reference released. That ordering is the
// GLSurfaceView contract: after surfaceDestroyed returns nothing may render into the window. A
// holder that does not answer in time does not get the window pulled out from under it - the
// reference is kept and released when the lease finally ends - and Detach returns TimedOut, by
// name, so the UI thread is never held longer than its bound.
//
// THREADS. Every method is thread-safe. The hooks are called from the thread that called the
// method: acquire from Attach's caller, release from Detach's / EndLease's caller, requestGeometry
// from the apply thread (AcquireFor), the lost hook from Detach's caller WITH THIS OBJECT'S LOCK
// HELD - so it must not block and must not call back into this object (ServerLoop's sets an atomic
// and rings its doorbell).

#pragma once
#include <Includes.h>

#include <condition_variable>
#include <mutex>
#include <vector>

namespace MobileGL::MG_Remote::Server {

    struct ServerDisplayHooks {
        // Take / drop one reference to `window` (ANativeWindow_acquire / ANativeWindow_release).
        void (*acquire)(void* user, void* window) = nullptr;
        void (*release)(void* user, void* window) = nullptr;
        // Ask the platform for a buffer geometry: w, h > 0 fixes it (setFixedSize), 0/0 hands it
        // back to the layout (setSizeFromLayout). Asynchronous: the new size arrives later as an
        // Attach() of the same window with the new extent (surfaceChanged). May be null.
        void (*requestGeometry)(void* user, Uint32 width, Uint32 height) = nullptr;
        void* user = nullptr;
    };

    // Called with the display's lock held: must not block, must not re-enter the display.
    using ServerWindowLostHook = void (*)(void* holder);
    // Asked about every 50 ms while AcquireFor waits; true ends the wait (Cancelled).
    using ServerWindowWaitCancel = Bool (*)(void* user);

    struct ServerWindowLease {
        void* window = nullptr;
        Uint32 width = 0;  // the window's extent when the lease was taken
        Uint32 height = 0;
        Uint64 generation = 0;
        // False when a size was requested and the window had not reached it within the geometry
        // grace: the lease is still granted, at the window's own size (logged by name).
        Bool sizeAsRequested = true;
    };

    enum class ServerWindowAcquire : Uint8 {
        Acquired,
        NoDisplay,       // this process owns no display (HasDisplay() is false)
        NoWindow,        // a display, but no window was attached within the wait
        Cancelled,       // the caller's cancel predicate said so
        Interrupted,     // Interrupt() (the in-process server is stopping)
        LeasedElsewhere, // another holder has the window (sessions are sequential: a bug if seen)
    };

    enum class ServerWindowDetach : Uint8 {
        NoWindow,          // nothing was attached
        Released,          // no session held the window; its reference is released
        ReleasedBySession, // the holding session released its surface in time, then the reference went
        TimedOut,          // the holder did not answer within the bound; released when it does
    };

    const char* ServerWindowAcquireName(ServerWindowAcquire result);
    const char* ServerWindowDetachName(ServerWindowDetach result);

    class ServerDisplay {
    public:
        // The apply thread's wait for a window (D3: bounded, ~10 s) and the UI thread's wait for the
        // holder to let go (D6: bounded, ~3 s). Once a window is attached, a requested size gets
        // kGeometryGraceMs more to arrive before the lease is granted at the window's own size.
        static constexpr Uint32 kDefaultAcquireTimeoutMs = 10000;
        static constexpr Uint32 kDefaultDetachTimeoutMs = 3000;
        static constexpr Uint32 kGeometryGraceMs = 2000;

        ServerDisplay() = default;
        ServerDisplay(const ServerDisplay&) = delete;
        ServerDisplay& operator=(const ServerDisplay&) = delete;

        // This process owns a display from here on (the in-process display server, once, before it
        // serves). Re-installing replaces the hooks.
        void Install(const ServerDisplayHooks& hooks);
        // The display is gone: detach whatever is attached (bounded, as Detach), wake every waiter
        // (NoDisplay), and forget the hooks.
        void Uninstall(Uint32 detachTimeoutMs = kDefaultDetachTimeoutMs);
        Bool HasDisplay() const;

        // surfaceCreated / surfaceChanged: `window` with its current buffer extent. The SAME window
        // again only updates the extent (and wakes a geometry wait); a DIFFERENT window first
        // detaches the old one (bounded) and then takes a reference to the new one.
        void Attach(void* window, Uint32 width, Uint32 height);
        // surfaceDestroyed. See the header block: asks the lease holder to release, waits up to
        // `timeoutMs`, then releases the window's reference (or leaves that to EndLease on timeout).
        ServerWindowDetach Detach(Uint32 timeoutMs = kDefaultDetachTimeoutMs);

        // The apply thread's side. Requests the geometry (w, h; 0/0 = the window's own), then waits
        // until a window is attached - and, for a requested size, has reached it or the geometry
        // grace ran out - and leases it to `holder`, whose `onLost` Detach will call. The same holder
        // may lease again while it holds the lease (a surface re-created at a new size).
        ServerWindowAcquire AcquireFor(Uint32 width, Uint32 height, Uint32 timeoutMs, void* holder,
                                       ServerWindowLostHook onLost, ServerWindowWaitCancel cancel,
                                       void* cancelUser, ServerWindowLease* out);
        // The holder has released everything that referenced the window (its backend surface, or the
        // whole backend). Wakes a waiting Detach; releases a reference Detach left behind on timeout.
        // A holder that holds no lease is a no-op.
        void EndLease(void* holder);
        // Wakes every AcquireFor waiter with Interrupted (the in-process server is stopping).
        void Interrupt();

        // Diagnostics, for logs and tests.
        Bool Attached() const;
        Bool Leased() const;
        Uint64 Generation() const;

    private:
        void ReleaseOutsideLock(const ServerDisplayHooks& hooks, void* window) const;
        // m_mutex held: an Attach reported `width`x`height` (see m_extentReports).
        void NoteExtentReportLocked(Uint32 width, Uint32 height);

        mutable std::mutex m_mutex;
        std::condition_variable m_cv;
        ServerDisplayHooks m_hooks{};
        Bool m_installed = false;
        void* m_window = nullptr;
        Uint32 m_width = 0;
        Uint32 m_height = 0;
        Uint64 m_generation = 0;
        Uint64 m_interrupts = 0;
        // P12 review fix (stale size). The geometry the last AcquireFor asked the platform for (0/0 =
        // the layout's), the extent the window last reported while the layout owned its size (0/0 =
        // not seen yet), and a count of every extent report. A 0/0 request that FOLLOWS a fixed one
        // is a change of size too - setSizeFromLayout answers asynchronously, like setFixedSize - so
        // it waits for the layout's extent instead of leasing the previous session's fixed one.
        Uint32 m_requestedWidth = 0;
        Uint32 m_requestedHeight = 0;
        Uint32 m_layoutWidth = 0;
        Uint32 m_layoutHeight = 0;
        Uint64 m_extentReports = 0;
        void* m_leaseHolder = nullptr;
        ServerWindowLostHook m_onLost = nullptr;
        Bool m_lostRequested = false;
        // References a timed-out Detach could not release yet; EndLease releases them.
        std::vector<void*> m_releaseAfterLease;
    };

    // The process's one display (leaked at exit, like every MG_Remote singleton).
    ServerDisplay& ServerDisplayInstance();

#if defined(__ANDROID__)
    // The ANativeWindow half of the hooks (acquire / release); the JNI glue supplies the geometry
    // up-call and its user pointer. Android-only: compiled by the APK build, never on the host.
    ServerDisplayHooks AndroidNativeWindowHooks(void (*requestGeometry)(void* user, Uint32 width, Uint32 height),
                                                void* user);
#endif

} // namespace MobileGL::MG_Remote::Server
