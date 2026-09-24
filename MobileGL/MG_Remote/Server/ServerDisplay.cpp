// MobileGL - MobileGL/MG_Remote/Server/ServerDisplay.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P12 (on-screen server window), D3/D6. See ServerDisplay.h.

#include "ServerDisplay.h"

#include <MG_Util/Debug/Log.h>

#include <algorithm>
#include <chrono>
#include <utility>

#if defined(__ANDROID__)
#include <android/native_window.h>
#endif

namespace MobileGL::MG_Remote::Server {

    namespace {
        using Clock = std::chrono::steady_clock;

        // How often a waiting AcquireFor re-asks its cancel predicate. The predicate is the apply
        // thread's own stop / latch state, which nobody signals through this object's condition.
        constexpr auto kCancelPollInterval = std::chrono::milliseconds(50);
    } // namespace

    const char* ServerWindowAcquireName(ServerWindowAcquire result) {
        switch (result) {
        case ServerWindowAcquire::Acquired: return "Acquired";
        case ServerWindowAcquire::NoDisplay: return "NoDisplay";
        case ServerWindowAcquire::NoWindow: return "NoWindow";
        case ServerWindowAcquire::Cancelled: return "Cancelled";
        case ServerWindowAcquire::Interrupted: return "Interrupted";
        case ServerWindowAcquire::LeasedElsewhere: return "LeasedElsewhere";
        }
        return "<unknown ServerWindowAcquire>";
    }

    const char* ServerWindowDetachName(ServerWindowDetach result) {
        switch (result) {
        case ServerWindowDetach::NoWindow: return "NoWindow";
        case ServerWindowDetach::Released: return "Released";
        case ServerWindowDetach::ReleasedBySession: return "ReleasedBySession";
        case ServerWindowDetach::TimedOut: return "TimedOut";
        }
        return "<unknown ServerWindowDetach>";
    }

    void ServerDisplay::ReleaseOutsideLock(const ServerDisplayHooks& hooks, void* window) const {
        if (window != nullptr && hooks.release != nullptr) hooks.release(hooks.user, window);
    }

    void ServerDisplay::NoteExtentReportLocked(Uint32 width, Uint32 height) {
        ++m_extentReports;
        // Only a report made while the layout owns the size says what the layout's size is.
        if (m_requestedWidth == 0 && m_requestedHeight == 0) {
            m_layoutWidth = width;
            m_layoutHeight = height;
        }
    }

    void ServerDisplay::Install(const ServerDisplayHooks& hooks) {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_hooks = hooks;
            m_installed = true;
        }
        m_cv.notify_all();
        MGLOG_I("MG_Remote server: this process owns a display (ServerDisplay installed); a ServerOwned "
                "window surface is served on its window");
    }

    void ServerDisplay::Uninstall(Uint32 detachTimeoutMs) {
        (void)Detach(detachTimeoutMs);
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_installed = false;
            // The hooks stay until every reference they took is released: a timed-out Detach may
            // still owe EndLease a release through them.
            if (m_releaseAfterLease.empty()) m_hooks = ServerDisplayHooks{};
        }
        m_cv.notify_all();
        MGLOG_I("MG_Remote server: ServerDisplay uninstalled; this process owns no display");
    }

    Bool ServerDisplay::HasDisplay() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_installed;
    }

    void ServerDisplay::Attach(void* window, Uint32 width, Uint32 height) {
        if (window == nullptr) return;
        std::unique_lock<std::mutex> lock(m_mutex);
        if (window == m_window) {
            // surfaceChanged: the same window at a new extent. This is also how a geometry request
            // is answered, so a waiting AcquireFor is woken.
            m_width = width;
            m_height = height;
            NoteExtentReportLocked(width, height);
            lock.unlock();
            m_cv.notify_all();
            MGLOG_I("MG_Remote server: server window %p is now %ux%u", window, width, height);
            return;
        }
        if (m_window != nullptr) {
            // A new window while one is still attached - the platform skipped the destroy. The old
            // one is detached exactly as surfaceDestroyed would have done it, bounded.
            lock.unlock();
            const ServerWindowDetach detached = Detach(kDefaultDetachTimeoutMs);
            MGLOG_W("MG_Remote server: server window %p attached while another was still attached; the "
                    "old one was detached first (%s)",
                    window, ServerWindowDetachName(detached));
            lock.lock();
        }
        if (m_hooks.acquire != nullptr) m_hooks.acquire(m_hooks.user, window);
        m_window = window;
        m_width = width;
        m_height = height;
        NoteExtentReportLocked(width, height);
        ++m_generation;
        const Uint64 generation = m_generation;
        lock.unlock();
        m_cv.notify_all();
        MGLOG_I("MG_Remote server: server window %p attached at %ux%u (generation %llu)", window, width, height,
                static_cast<unsigned long long>(generation));
    }

    ServerWindowDetach ServerDisplay::Detach(Uint32 timeoutMs) {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_window == nullptr) return ServerWindowDetach::NoWindow;
        void* const window = m_window;
        m_window = nullptr;
        m_width = 0;
        m_height = 0;
        ++m_generation;
        ServerWindowDetach result = ServerWindowDetach::Released;
        if (m_leaseHolder != nullptr) {
            // A live on-screen session renders into this window. Ask it to let go - the hook runs
            // under this lock, so the holder cannot end its lease (and go away) between being read
            // here and being called - then wait for EndLease, bounded.
            if (!m_lostRequested) {
                m_lostRequested = true;
                if (m_onLost != nullptr) m_onLost(m_leaseHolder);
            }
            const Bool released = m_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                                [this] { return m_leaseHolder == nullptr; });
            if (!released) {
                // NOT RELEASED UNDER IT. The holder may still reference the window (Magma keeps the
                // raw pointer until the swapchain is built), so the reference stays until it ends
                // its lease. The UI thread is not held past its bound either way.
                m_releaseAfterLease.push_back(window);
                lock.unlock();
                m_cv.notify_all();
                MGLOG_E("MG_Remote server: surfaceDestroyed - the on-screen session did not release server "
                        "window %p within %u ms; its reference is kept until it does (Detach TimedOut)",
                        window, timeoutMs);
                return ServerWindowDetach::TimedOut;
            }
            result = ServerWindowDetach::ReleasedBySession;
        }
        const ServerDisplayHooks hooks = m_hooks;
        lock.unlock();
        m_cv.notify_all();
        ReleaseOutsideLock(hooks, window);
        MGLOG_I("MG_Remote server: server window %p detached (%s)", window, ServerWindowDetachName(result));
        return result;
    }

    ServerWindowAcquire ServerDisplay::AcquireFor(Uint32 width, Uint32 height, Uint32 timeoutMs, void* holder,
                                                  ServerWindowLostHook onLost, ServerWindowWaitCancel cancel,
                                                  void* cancelUser, ServerWindowLease* out) {
        ServerDisplayHooks hooks{};
        Uint64 interrupts = 0;
        const Bool sizeRequested = width != 0 && height != 0;
        // P12 review fix (stale size): a 0/0 request after a fixed one hands the size BACK to the
        // layout, and until the platform has done so the window still reports the fixed size.
        Bool backToLayout = false;
        Uint32 previousWidth = 0;
        Uint32 previousHeight = 0;
        Uint64 reportsAtRequest = 0;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_installed) return ServerWindowAcquire::NoDisplay;
            if (m_leaseHolder != nullptr && m_leaseHolder != holder) return ServerWindowAcquire::LeasedElsewhere;
            hooks = m_hooks;
            interrupts = m_interrupts;
            previousWidth = m_requestedWidth;
            previousHeight = m_requestedHeight;
            backToLayout = !sizeRequested && (previousWidth != 0 || previousHeight != 0);
            m_requestedWidth = sizeRequested ? width : 0u;
            m_requestedHeight = sizeRequested ? height : 0u;
            reportsAtRequest = m_extentReports;
        }
        // The geometry request is an up-call into the platform (Java), made without the lock: its
        // answer comes back as an Attach() of the same window, which needs the lock.
        if (hooks.requestGeometry != nullptr) hooks.requestGeometry(hooks.user, width, height);
        // What "the size asked for" means on each path. A fixed size: the window reports it. The
        // layout's, after a fixed one: the window reports the layout's extent it reported before, or
        // - the layout moved meanwhile (a rotation), or was never seen - reports anything but the old
        // fixed size after this request. The layout's, with no fixed size in effect: whatever it is.
        const auto sizeReached = [&] {
            if (sizeRequested) return m_width == width && m_height == height;
            if (!backToLayout) return true;
            if (m_layoutWidth != 0 && m_width == m_layoutWidth && m_height == m_layoutHeight) return true;
            return m_extentReports != reportsAtRequest && (m_width != previousWidth || m_height != previousHeight);
        };

        std::unique_lock<std::mutex> lock(m_mutex);
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        Bool attachedSeen = false;
        auto geometryDeadline = deadline;
        for (;;) {
            if (!m_installed) return ServerWindowAcquire::NoDisplay;
            if (m_interrupts != interrupts) return ServerWindowAcquire::Interrupted;
            if (cancel != nullptr && cancel(cancelUser)) return ServerWindowAcquire::Cancelled;
            if (m_leaseHolder != nullptr && m_leaseHolder != holder) return ServerWindowAcquire::LeasedElsewhere;
            const auto now = Clock::now();
            if (m_window != nullptr) {
                if (!attachedSeen) {
                    attachedSeen = true;
                    geometryDeadline = std::min(deadline, now + std::chrono::milliseconds(kGeometryGraceMs));
                }
                const Bool sizeOk = sizeReached();
                if (sizeOk || now >= geometryDeadline) {
                    m_leaseHolder = holder;
                    m_onLost = onLost;
                    m_lostRequested = false;
                    if (out != nullptr) {
                        out->window = m_window;
                        out->width = m_width;
                        out->height = m_height;
                        out->generation = m_generation;
                        out->sizeAsRequested = sizeOk;
                    }
                    if (!sizeOk && sizeRequested) {
                        MGLOG_W("MG_Remote server: the server window is %ux%u, not the %ux%u requested, after "
                                "%u ms of geometry grace; the surface is created at the window's size and the "
                                "client is told so",
                                m_width, m_height, width, height, kGeometryGraceMs);
                    } else if (!sizeOk) {
                        MGLOG_W("MG_Remote server: the server window still reports %ux%u - the previous fixed "
                                "size - %u ms after its size was handed back to the layout; the surface is created "
                                "at that size and the client is told so",
                                m_width, m_height, kGeometryGraceMs);
                    }
                    return ServerWindowAcquire::Acquired;
                }
            } else {
                attachedSeen = false;
                geometryDeadline = deadline;
            }
            if (now >= deadline) return ServerWindowAcquire::NoWindow;
            const auto wake = std::min({deadline, geometryDeadline, now + kCancelPollInterval});
            m_cv.wait_until(lock, wake);
        }
    }

    void ServerDisplay::EndLease(void* holder) {
        std::vector<void*> releases;
        ServerDisplayHooks hooks{};
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (holder == nullptr || m_leaseHolder != holder) return;
            m_leaseHolder = nullptr;
            m_onLost = nullptr;
            m_lostRequested = false;
            releases.swap(m_releaseAfterLease);
            hooks = m_hooks;
            // An Uninstall that ran while this lease still owed releases kept the hooks for them.
            if (!m_installed) m_hooks = ServerDisplayHooks{};
        }
        m_cv.notify_all();
        for (void* window : releases) {
            ReleaseOutsideLock(hooks, window);
            MGLOG_I("MG_Remote server: server window %p released after its session let go (a late Detach)",
                    window);
        }
    }

    void ServerDisplay::Interrupt() {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            ++m_interrupts;
        }
        m_cv.notify_all();
    }

    Bool ServerDisplay::Attached() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_window != nullptr;
    }

    Bool ServerDisplay::Leased() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_leaseHolder != nullptr;
    }

    Uint64 ServerDisplay::Generation() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_generation;
    }

    ServerDisplay& ServerDisplayInstance() {
        // ID-8: leak at exit, like every MG_Remote singleton.
        static ServerDisplay& instance = *new ServerDisplay{};
        return instance;
    }

#if defined(__ANDROID__)
    namespace {
        void AcquireAndroidWindow(void*, void* window) {
            ANativeWindow_acquire(static_cast<ANativeWindow*>(window));
        }
        void ReleaseAndroidWindow(void*, void* window) {
            ANativeWindow_release(static_cast<ANativeWindow*>(window));
        }
    } // namespace

    ServerDisplayHooks AndroidNativeWindowHooks(void (*requestGeometry)(void* user, Uint32 width, Uint32 height),
                                                void* user) {
        ServerDisplayHooks hooks;
        hooks.acquire = &AcquireAndroidWindow;
        hooks.release = &ReleaseAndroidWindow;
        hooks.requestGeometry = requestGeometry;
        hooks.user = user;
        return hooks;
    }
#endif

} // namespace MobileGL::MG_Remote::Server
