// MobileGL - MobileGL/MG_Remote/Server/ServerDisplay.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The server's own on-screen window, held process-wide between the UI thread that owns its
// lifecycle (Attach / Detach on surfaceCreated, surfaceChanged, surfaceDestroyed) and the apply
// thread that renders into it under a lease (AcquireFor / EndLease). Only the in-process display
// server calls Install(); elsewhere a ServerOwned surface is refused (NoServerDisplay). The window
// is an opaque `void*` driven through ServerDisplayHooks, so no platform header is needed here.
// Nothing may render into the window after surfaceDestroyed returns, so Detach asks the holder to
// let go and waits, bounded, for EndLease; on timeout it returns TimedOut and EndLease releases
// the window later. A lease marked ending is waited for, not refused. See ARCHITECTURE.md §15.2.

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
    // A new window was attached while `holder` was suspended (SuspendLease). Same rules as the lost
    // hook: called with the display's lock held.
    using ServerWindowAttachedHook = void (*)(void* holder);
    // The leased window changed size (surfaceChanged of the SAME window: the app's layout moved it).
    // Same rules: called with the display's lock held; the holder reads the new extent with WindowExtent.
    using ServerWindowResizedHook = void (*)(void* holder);
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
        // P14 S2: NOT A BUG ANY MORE. Several sessions share one process and the display's window
        // is one window, so a second ON-SCREEN session asking for it is refused by name here
        // (ServerLoop logs the holder); offscreen sessions take no lease and are unaffected. A holder
        // whose session is ending (MarkLeaseEnding) is waited for instead, and this is the answer
        // only when its lease outlasted the wait.
        LeasedElsewhere, // another session in this process holds the window: see the note above
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
        // may lease again while it holds the lease (a surface re-created at a new size). `onResized`, when
        // given, is called whenever the leased window changes size while the lease lasts.
        ServerWindowAcquire AcquireFor(Uint32 width, Uint32 height, Uint32 timeoutMs, void* holder,
                                       ServerWindowLostHook onLost, ServerWindowWaitCancel cancel,
                                       void* cancelUser, ServerWindowLease* out,
                                       ServerWindowResizedHook onResized = nullptr);
        // THE WINDOW'S EXTENT AS THE PLATFORM LAST REPORTED IT (surfaceCreated / surfaceChanged): the one
        // source of truth for the size a session publishes to its client. False unless `holder` holds
        // the lease on an attached window.
        Bool WindowExtent(void* holder, void** window, Uint32* width, Uint32* height) const;
        // The holder has released everything that referenced the window (its backend surface, or the
        // whole backend). Wakes a waiting Detach; releases a reference Detach left behind on timeout.
        // A holder that holds no lease is a no-op.
        void EndLease(void* holder);
        // `holder`'s session is ending (it latched, or it is stopping): its lease ends once its
        // backend has let go of the window, and another holder's AcquireFor waits for that rather
        // than being refused (see the header block). A no-op for a holder that holds no lease; the
        // mark ends with the lease.
        void MarkLeaseEnding(void* holder);
        // EndLease for a holder that only SUSPENDED its use of the window (its backend let go of the
        // window and kept everything else). Also registers `onAttached`, which the next Attach of a
        // new window calls - at once when a window is already attached. The registration ends when
        // the same holder's AcquireFor succeeds, or at CancelResume.
        void SuspendLease(void* holder, ServerWindowAttachedHook onAttached);
        // The holder is going away: no attached hook is called for it once this returns.
        void CancelResume(void* holder);
        // Wakes every AcquireFor waiter with Interrupted (the in-process server is stopping).
        void Interrupt();

        // Diagnostics, for logs and tests.
        Bool Attached() const;
        Bool Leased() const;
        // The lease is held by a holder whose session is ending (MarkLeaseEnding).
        Bool LeaseEnding() const;
        Uint64 Generation() const;
        // How many holders wait for a window to resume on (SuspendLease).
        Uint32 SuspendedHolders() const;

    private:
        void ReleaseOutsideLock(const ServerDisplayHooks& hooks, void* window) const;
        // m_mutex held: ends `holder`'s lease (false when it holds none); the references a timed-out
        // Detach left behind are moved to `releases`, to be released outside the lock with `hooks`.
        Bool EndLeaseLocked(void* holder, std::vector<void*>* releases, ServerDisplayHooks* hooks);
        void ReleaseAfterLease(const ServerDisplayHooks& hooks, const std::vector<void*>& releases) const;
        // m_mutex held: a new window is attached; every suspended holder is told.
        void NotifySuspendedLocked() const;
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
        ServerWindowResizedHook m_onResized = nullptr;
        Bool m_lostRequested = false;
        // The holder's session is ending (MarkLeaseEnding); cleared with the lease.
        Bool m_leaseEnding = false;
        // References a timed-out Detach could not release yet; EndLease releases them.
        std::vector<void*> m_releaseAfterLease;
        // SuspendLease's registrations, told about the next new window.
        struct SuspendedHolder {
            void* holder = nullptr;
            ServerWindowAttachedHook onAttached = nullptr;
        };
        std::vector<SuspendedHolder> m_suspended;
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
