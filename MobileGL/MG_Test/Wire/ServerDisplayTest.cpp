// MobileGL - MobileGL/MG_Test/Wire/ServerDisplayTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P12 (on-screen server window), D3/D6: the server's own window, held by ServerDisplay, driven here
// with a FAKE window handle and counting hooks - the host has no ANativeWindow, and that is exactly
// why the holder takes the window as an opaque handle with callbacks.
//
// What is proven: a process that installed nothing has no display (the host server, the exec'd
// supervisor); a wait for a window is bounded and wakes on the attach and on the requested size;
// the geometry request reaches the platform hook with the size asked for (0/0 = the window's own);
// Detach of a window a session holds asks the holder to let go, BLOCKS until it did, and releases
// the reference only after it; a holder that never answers does not have the window released under
// it; Detach of a window nobody holds releases it at once; Interrupt and a cancel predicate end a
// wait by name.

#include <MG_Remote/Server/ServerDisplay.h>
#include <MG_Remote/Server/EmbeddedServer.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace MobileGL;
using namespace MobileGL::MG_Remote::Server;

namespace {

    // The platform half, faked: every acquire/release/geometry request is recorded, in order.
    struct FakePlatform {
        std::mutex mutex;
        std::vector<std::string> events;
        std::atomic<int> acquires{0};
        std::atomic<int> releases{0};
        std::atomic<int> geometryRequests{0};
        Uint32 lastWidth = 0;
        Uint32 lastHeight = 0;

        void Note(const std::string& event) {
            const std::lock_guard<std::mutex> lock(mutex);
            events.push_back(event);
        }
        std::vector<std::string> Events() {
            const std::lock_guard<std::mutex> lock(mutex);
            return events;
        }
    };

    void FakeAcquire(void* user, void*) {
        auto* platform = static_cast<FakePlatform*>(user);
        ++platform->acquires;
        platform->Note("acquire");
    }
    void FakeRelease(void* user, void*) {
        auto* platform = static_cast<FakePlatform*>(user);
        ++platform->releases;
        platform->Note("release");
    }
    void FakeGeometry(void* user, Uint32 width, Uint32 height) {
        auto* platform = static_cast<FakePlatform*>(user);
        platform->lastWidth = width;
        platform->lastHeight = height;
        ++platform->geometryRequests;
        platform->Note("geometry");
    }

    ServerDisplayHooks HooksFor(FakePlatform& platform) {
        ServerDisplayHooks hooks;
        hooks.acquire = &FakeAcquire;
        hooks.release = &FakeRelease;
        hooks.requestGeometry = &FakeGeometry;
        hooks.user = &platform;
        return hooks;
    }

    // The session side of a lease: what ServerLoop's lost hook does is set a flag its apply thread
    // reads; here a "session" thread does the same and ends the lease when asked.
    struct FakeSession {
        ServerDisplay* display = nullptr;
        FakePlatform* platform = nullptr;
        std::atomic<bool> lostRequested{false};
        std::atomic<int> lostCalls{0};
        bool answer = true; // false: never ends its lease (a wedged apply thread)
        std::thread thread;

        static void OnLost(void* self) {
            auto* session = static_cast<FakeSession*>(self);
            ++session->lostCalls;
            session->lostRequested.store(true);
        }
        void Serve() {
            thread = std::thread([this] {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                while (!lostRequested.load() && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                if (!answer || !lostRequested.load()) return;
                // "The backend lets go of the window" - then the lease ends.
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                platform->Note("session released its surface");
                display->EndLease(this);
            });
        }
        ~FakeSession() {
            if (thread.joinable()) thread.join();
        }
    };

    int g_windowStorage[4];
    void* FakeWindow(int index) { return &g_windowStorage[index]; }

} // namespace

// The public embedding ABI must preserve the same ownership rules for an APK that supplies its
// own Surface, including a late apply-thread release after its UI thread has stopped waiting.
class EmbeddedServerTest : public ::testing::Test {
protected:
    FakePlatform platform;

    mobilegl_server_display_hooks Hooks() {
        return {&FakeAcquire, &FakeRelease, &FakeGeometry, &platform};
    }
    void TearDown() override {
        ServerDisplayInstance().EndLease(this);
        (void)mobilegl_server_display_uninstall(0);
    }
};

TEST_F(EmbeddedServerTest, AHostsReferenceIsRetainedAndGeometryUpdatesDoNotRetainAgain) {
    const auto hooks = Hooks();
    ASSERT_EQ(mobilegl_server_display_install(&hooks), MOBILEGL_SERVER_DISPLAY_OK);
    ASSERT_EQ(mobilegl_server_display_attach(FakeWindow(0), 32, 24), MOBILEGL_SERVER_DISPLAY_OK);
    ASSERT_EQ(mobilegl_server_display_attach(FakeWindow(0), 64, 48), MOBILEGL_SERVER_DISPLAY_OK);
    EXPECT_EQ(platform.acquires.load(), 1);
    ServerWindowLease lease;
    ASSERT_EQ(ServerDisplayInstance().AcquireFor(64, 48, 100, this, nullptr, nullptr, nullptr, &lease),
              ServerWindowAcquire::Acquired);
    EXPECT_EQ(platform.lastWidth, 64u);
    EXPECT_EQ(platform.lastHeight, 48u);
    EXPECT_EQ(lease.width, 64u);
    EXPECT_EQ(lease.height, 48u);
    ServerDisplayInstance().EndLease(this);
    EXPECT_EQ(mobilegl_server_display_detach(100), MOBILEGL_SERVER_WINDOW_RELEASED);
    EXPECT_EQ(platform.releases.load(), 1);
    EXPECT_EQ(mobilegl_server_display_uninstall(100), MOBILEGL_SERVER_WINDOW_NO_WINDOW);
}

TEST_F(EmbeddedServerTest, ATimeoutKeepsTheOriginalHooksUntilTheBackendLetsGo) {
    const auto hooks = Hooks();
    ASSERT_EQ(mobilegl_server_display_install(&hooks), MOBILEGL_SERVER_DISPLAY_OK);
    ASSERT_EQ(mobilegl_server_display_attach(FakeWindow(0), 64, 48), MOBILEGL_SERVER_DISPLAY_OK);
    ServerWindowLease lease;
    // The holder deliberately has no lost callback: it is a wedged apply thread. Uninstall must
    // remain bounded and retain the window until this holder explicitly releases its lease.
    ASSERT_EQ(ServerDisplayInstance().AcquireFor(0, 0, 100, this, nullptr, nullptr, nullptr, &lease),
              ServerWindowAcquire::Acquired);
    EXPECT_EQ(mobilegl_server_display_uninstall(1), MOBILEGL_SERVER_WINDOW_TIMED_OUT);
    EXPECT_FALSE(ServerDisplayInstance().HasDisplay());
    EXPECT_EQ(platform.releases.load(), 0);
    EXPECT_EQ(mobilegl_server_display_install(&hooks), MOBILEGL_SERVER_DISPLAY_ALREADY_INSTALLED);
    ServerDisplayInstance().EndLease(this);
    EXPECT_EQ(platform.releases.load(), 1);
    EXPECT_EQ(mobilegl_server_display_install(&hooks), MOBILEGL_SERVER_DISPLAY_OK);
}

TEST_F(EmbeddedServerTest, AnOwnerCannotReplaceLiveReferenceHooksOrAttachBeforeInstall) {
    const auto hooks = Hooks();
    EXPECT_EQ(mobilegl_server_display_attach(FakeWindow(0), 64, 48), MOBILEGL_SERVER_DISPLAY_NOT_INSTALLED);
    EXPECT_EQ(mobilegl_server_display_install(nullptr), MOBILEGL_SERVER_DISPLAY_INVALID_ARGUMENT);
    mobilegl_server_display_hooks incomplete{};
    EXPECT_EQ(mobilegl_server_display_install(&incomplete), MOBILEGL_SERVER_DISPLAY_INVALID_ARGUMENT);
    ASSERT_EQ(mobilegl_server_display_install(&hooks), MOBILEGL_SERVER_DISPLAY_OK);
    EXPECT_EQ(mobilegl_server_display_install(&hooks), MOBILEGL_SERVER_DISPLAY_ALREADY_INSTALLED);
    EXPECT_EQ(mobilegl_server_display_attach(nullptr, 64, 48), MOBILEGL_SERVER_DISPLAY_INVALID_ARGUMENT);
}

#if !defined(__ANDROID__)
TEST_F(EmbeddedServerTest, AndroidWindowsAreRefusedByNameOnOtherPlatforms) {
    EXPECT_EQ(mobilegl_server_display_install_android(nullptr, nullptr), MOBILEGL_SERVER_DISPLAY_UNSUPPORTED_PLATFORM);
    EXPECT_EQ(mobilegl_server_display_attach_android(nullptr, 64, 48), MOBILEGL_SERVER_DISPLAY_UNSUPPORTED_PLATFORM);
    EXPECT_FALSE(ServerDisplayInstance().HasDisplay());
}
#endif

// Nobody installed a display: HasDisplay is false and a wait for a window answers NoDisplay at once
// - the host server's and the exec'd supervisor's shape, and what makes a ServerOwned request a named
// refusal there.
TEST(ServerDisplayTest, AProcessThatInstalledNothingHasNoDisplay) {
    ServerDisplay display;
    EXPECT_FALSE(display.HasDisplay());
    ServerWindowLease lease;
    const auto started = std::chrono::steady_clock::now();
    EXPECT_EQ(display.AcquireFor(64, 48, 5000, this, nullptr, nullptr, nullptr, &lease), ServerWindowAcquire::NoDisplay);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(1000))
        << "a process with no display must not wait for a window";
    // And the process singleton is that process: nothing on the host installs one.
    EXPECT_FALSE(ServerDisplayInstance().HasDisplay());
}

// D3: the wait for a window is BOUNDED, and the platform was asked for the size first.
TEST(ServerDisplayTest, AWaitForAWindowThatNeverComesEndsByNameAfterItsBound) {
    FakePlatform platform;
    ServerDisplay display;
    display.Install(HooksFor(platform));
    EXPECT_TRUE(display.HasDisplay());
    ServerWindowLease lease;
    const auto started = std::chrono::steady_clock::now();
    EXPECT_EQ(display.AcquireFor(640, 480, 200, this, nullptr, nullptr, nullptr, &lease), ServerWindowAcquire::NoWindow);
    const auto waited = std::chrono::steady_clock::now() - started;
    EXPECT_GE(waited, std::chrono::milliseconds(150));
    EXPECT_LT(waited, std::chrono::milliseconds(3000));
    EXPECT_EQ(platform.geometryRequests.load(), 1);
    EXPECT_EQ(platform.lastWidth, 640u);
    EXPECT_EQ(platform.lastHeight, 480u);
    EXPECT_FALSE(display.Leased());
}

// D3: a window attached while the apply thread waits wakes it; 0/0 takes the window at its own size
// (setSizeFromLayout), and the reference was taken by the attach.
TEST(ServerDisplayTest, AnAttachWakesTheWaitAndTheLeaseIsTheWindowsOwnSizeForZeroByZero) {
    FakePlatform platform;
    ServerDisplay display;
    display.Install(HooksFor(platform));
    std::thread attacher([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        display.Attach(FakeWindow(0), 1080, 2400);
    });
    ServerWindowLease lease;
    EXPECT_EQ(display.AcquireFor(0, 0, 5000, this, nullptr, nullptr, nullptr, &lease), ServerWindowAcquire::Acquired);
    attacher.join();
    EXPECT_EQ(lease.window, FakeWindow(0));
    EXPECT_EQ(lease.width, 1080u);
    EXPECT_EQ(lease.height, 2400u);
    EXPECT_TRUE(lease.sizeAsRequested);
    EXPECT_EQ(platform.lastWidth, 0u) << "0/0 is the request for the layout's own size";
    EXPECT_EQ(platform.acquires.load(), 1);
    EXPECT_TRUE(display.Leased());
    display.EndLease(this);
    EXPECT_FALSE(display.Leased());
    EXPECT_EQ(platform.releases.load(), 0) << "ending a lease is not detaching the window";
}

// D3: a requested size is waited for - the surfaceChanged that answers the geometry request is an
// Attach of the SAME window at the new size, and it is what grants the lease.
TEST(ServerDisplayTest, ARequestedSizeIsWaitedForUntilTheWindowReportsIt) {
    FakePlatform platform;
    ServerDisplay display;
    display.Install(HooksFor(platform));
    display.Attach(FakeWindow(0), 1080, 2400);
    std::thread resizer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        display.Attach(FakeWindow(0), 640, 480);
    });
    ServerWindowLease lease;
    const auto started = std::chrono::steady_clock::now();
    EXPECT_EQ(display.AcquireFor(640, 480, 5000, this, nullptr, nullptr, nullptr, &lease),
              ServerWindowAcquire::Acquired);
    resizer.join();
    EXPECT_GE(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(100))
        << "the lease was granted before the window reached the requested size";
    EXPECT_EQ(lease.width, 640u);
    EXPECT_EQ(lease.height, 480u);
    EXPECT_TRUE(lease.sizeAsRequested);
    EXPECT_EQ(platform.acquires.load(), 1) << "the same window re-attached must not take a second reference";
    display.EndLease(this);
}

// P12 review fix (stale size). A session that asks for the window's OWN size after a session that fixed
// it must get the layout's size back, not the fixed one the window still reports until setSizeFromLayout
// lands. Red with AcquireFor granting 0/0 at once (the first version): the lease is 640x480 and comes
// back before the layout's surfaceChanged.
TEST(ServerDisplayTest, TheWindowsOwnSizeAfterAFixedSizeIsTheLayoutsNotTheStaleFixedOne) {
    FakePlatform platform;
    ServerDisplay display;
    display.Install(HooksFor(platform));
    display.Attach(FakeWindow(0), 1080, 2400); // surfaceCreated at the layout's size
    // Session 1 fixes 640x480 (setFixedSize answers with surfaceChanged) and ends.
    std::thread fixer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        display.Attach(FakeWindow(0), 640, 480);
    });
    ServerWindowLease lease;
    ASSERT_EQ(display.AcquireFor(640, 480, 5000, this, nullptr, nullptr, nullptr, &lease), ServerWindowAcquire::Acquired);
    fixer.join();
    display.EndLease(this);
    // Session 2 asks for the window's own size: setSizeFromLayout answers later, at the layout's size.
    std::thread layout([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        display.Attach(FakeWindow(0), 1080, 2400);
    });
    const auto started = std::chrono::steady_clock::now();
    ASSERT_EQ(display.AcquireFor(0, 0, 5000, this, nullptr, nullptr, nullptr, &lease), ServerWindowAcquire::Acquired);
    const auto waited = std::chrono::steady_clock::now() - started;
    layout.join();
    EXPECT_EQ(platform.lastWidth, 0u) << "0/0 is the setSizeFromLayout request";
    EXPECT_EQ(lease.width, 1080u) << "the lease took the previous session's fixed size";
    EXPECT_EQ(lease.height, 2400u);
    EXPECT_TRUE(lease.sizeAsRequested);
    EXPECT_GE(waited, std::chrono::milliseconds(100)) << "granted before the layout's size came back";
    EXPECT_LT(waited, std::chrono::milliseconds(ServerDisplay::kGeometryGraceMs)) << "waited out the whole grace";
    display.EndLease(this);
    // And a 0/0 request with the layout already owning the size is granted at once, as before.
    const auto again = std::chrono::steady_clock::now();
    ASSERT_EQ(display.AcquireFor(0, 0, 5000, this, nullptr, nullptr, nullptr, &lease), ServerWindowAcquire::Acquired);
    EXPECT_LT(std::chrono::steady_clock::now() - again, std::chrono::milliseconds(100));
    EXPECT_EQ(lease.width, 1080u);
    display.EndLease(this);
}

// D6, THE CONTRACT: Detach of a window a session holds asks the holder to let go, WAITS until it did,
// and only then releases the window's reference. Red with Detach's wait deleted: the release is
// recorded before the session released its surface (and Detach answers Released, not
// ReleasedBySession).
TEST(ServerDisplayTest, DetachOfALeasedWindowWaitsForTheSessionAndReleasesOnlyAfterIt) {
    FakePlatform platform;
    ServerDisplay display;
    display.Install(HooksFor(platform));
    display.Attach(FakeWindow(1), 800, 600);
    FakeSession session;
    session.display = &display;
    session.platform = &platform;
    ServerWindowLease lease;
    ASSERT_EQ(display.AcquireFor(800, 600, 1000, &session, &FakeSession::OnLost, nullptr, nullptr, &lease),
              ServerWindowAcquire::Acquired);
    session.Serve();
    EXPECT_EQ(display.Detach(3000), ServerWindowDetach::ReleasedBySession);
    EXPECT_EQ(session.lostCalls.load(), 1);
    EXPECT_EQ(platform.releases.load(), 1);
    const auto events = platform.Events();
    ASSERT_GE(events.size(), 3u);
    EXPECT_EQ(events[events.size() - 2], "session released its surface");
    EXPECT_EQ(events.back(), "release") << "the window was released before the session let go of it";
    EXPECT_FALSE(display.Attached());
    EXPECT_FALSE(display.Leased());
}

// D6: a holder that does not answer within the bound does NOT have the window released under it -
// Detach returns TimedOut by name and the reference goes when the lease finally ends.
TEST(ServerDisplayTest, ADetachThatTimesOutKeepsTheReferenceUntilTheLeaseEnds) {
    FakePlatform platform;
    ServerDisplay display;
    display.Install(HooksFor(platform));
    display.Attach(FakeWindow(2), 800, 600);
    FakeSession session;
    session.display = &display;
    session.platform = &platform;
    session.answer = false;
    ServerWindowLease lease;
    ASSERT_EQ(display.AcquireFor(800, 600, 1000, &session, &FakeSession::OnLost, nullptr, nullptr, &lease),
              ServerWindowAcquire::Acquired);
    const auto started = std::chrono::steady_clock::now();
    EXPECT_EQ(display.Detach(150), ServerWindowDetach::TimedOut);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(2000));
    EXPECT_EQ(platform.releases.load(), 0) << "the window was released under a session still holding it";
    display.EndLease(&session);
    EXPECT_EQ(platform.releases.load(), 1) << "the late EndLease did not release the kept reference";
}

// D6: "a session that never had a window is unaffected" - Detach of a window nobody holds releases it
// at once and calls no lost hook.
TEST(ServerDisplayTest, DetachOfAWindowNobodyHoldsReleasesItAtOnce) {
    FakePlatform platform;
    ServerDisplay display;
    display.Install(HooksFor(platform));
    EXPECT_EQ(display.Detach(), ServerWindowDetach::NoWindow);
    display.Attach(FakeWindow(3), 10, 10);
    EXPECT_EQ(display.Detach(3000), ServerWindowDetach::Released);
    EXPECT_EQ(platform.acquires.load(), 1);
    EXPECT_EQ(platform.releases.load(), 1);
}

// A second holder cannot take a window another session holds (sessions are sequential; seeing this
// is a bug, and it is named rather than shared).
TEST(ServerDisplayTest, AWindowLeasedToOneHolderIsNotLeasedToAnother) {
    FakePlatform platform;
    ServerDisplay display;
    display.Install(HooksFor(platform));
    display.Attach(FakeWindow(0), 10, 10);
    int first = 0;
    int second = 0;
    ServerWindowLease lease;
    ASSERT_EQ(display.AcquireFor(0, 0, 1000, &first, nullptr, nullptr, nullptr, &lease), ServerWindowAcquire::Acquired);
    EXPECT_EQ(display.AcquireFor(0, 0, 1000, &first, nullptr, nullptr, nullptr, &lease), ServerWindowAcquire::Acquired)
        << "the holder itself may lease again (a surface re-created at a new size)";
    EXPECT_EQ(display.AcquireFor(0, 0, 100, &second, nullptr, nullptr, nullptr, &lease),
              ServerWindowAcquire::LeasedElsewhere);
    display.EndLease(&first);
}

// The two ways a wait ends early, by name: the in-process server stopping (Interrupt), and the
// caller's own cancel predicate (ServerLoop's: the loop stopping, the session latched).
TEST(ServerDisplayTest, InterruptAndTheCancelPredicateEndAWaitByName) {
    FakePlatform platform;
    ServerDisplay display;
    display.Install(HooksFor(platform));
    std::thread interrupter([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        display.Interrupt();
    });
    ServerWindowLease lease;
    EXPECT_EQ(display.AcquireFor(0, 0, 5000, this, nullptr, nullptr, nullptr, &lease), ServerWindowAcquire::Interrupted);
    interrupter.join();

    static std::atomic<bool> cancelled{false};
    cancelled.store(false);
    std::thread canceller([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        cancelled.store(true);
    });
    const auto cancel = [](void*) -> Bool { return cancelled.load(); };
    EXPECT_EQ(display.AcquireFor(0, 0, 5000, this, nullptr, cancel, nullptr, &lease), ServerWindowAcquire::Cancelled);
    canceller.join();

    // And Uninstall: the display is gone, the waiter hears NoDisplay.
    std::thread uninstaller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        display.Uninstall();
    });
    EXPECT_EQ(display.AcquireFor(0, 0, 5000, this, nullptr, nullptr, nullptr, &lease), ServerWindowAcquire::NoDisplay);
    uninstaller.join();
    EXPECT_FALSE(display.HasDisplay());
}

// SUSPEND, NOT END. A holder that suspends (its backend let go of the window and kept the rest) ends
// its lease - a waiting Detach returns, the reference is released - and is told about the next NEW
// window: not about a geometry change of the same one, once per new window, until it leases a window
// again or cancels. Red with Attach not calling the hooks (attached stays 0), with AcquireFor keeping
// the registration (SuspendedHolders stays 1), or with CancelResume a no-op (the last window
// calls it again).
TEST(ServerDisplayTest, ASuspendedHolderIsToldAboutTheNextNewWindowUntilItLeasesOneOrCancels) {
    FakePlatform platform;
    ServerDisplay display;
    display.Install(HooksFor(platform));
    display.Attach(FakeWindow(0), 64, 48);
    static std::atomic<int> attached{0};
    attached.store(0);
    const auto onAttached = [](void*) { attached.fetch_add(1); };
    static std::atomic<int> lost{0};
    lost.store(0);
    const auto onLost = [](void*) { lost.fetch_add(1); };
    int holder = 0;
    ServerWindowLease lease;
    ASSERT_EQ(display.AcquireFor(64, 48, 100, &holder, onLost, nullptr, nullptr, &lease), ServerWindowAcquire::Acquired);

    // The screen goes off; the "apply thread" suspends instead of ending.
    std::thread session([&] {
        while (lost.load() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        display.SuspendLease(&holder, onAttached);
    });
    EXPECT_EQ(display.Detach(3000), ServerWindowDetach::ReleasedBySession);
    session.join();
    EXPECT_EQ(platform.releases.load(), 1);
    EXPECT_FALSE(display.Leased());
    EXPECT_EQ(display.SuspendedHolders(), 1u);
    EXPECT_EQ(attached.load(), 0) << "no window is up: nothing to resume on yet";

    // The screen comes back: a new window, then its size settling (the same window again).
    display.Attach(FakeWindow(1), 64, 48);
    EXPECT_EQ(attached.load(), 1);
    display.Attach(FakeWindow(1), 80, 60);
    EXPECT_EQ(attached.load(), 1) << "a geometry change of the same window is not a new window";

    // The holder leases it: the registration is over.
    ASSERT_EQ(display.AcquireFor(0, 0, 100, &holder, onLost, nullptr, nullptr, &lease), ServerWindowAcquire::Acquired);
    EXPECT_EQ(lease.window, FakeWindow(1));
    EXPECT_EQ(display.SuspendedHolders(), 0u);

    // Suspended again while a window is ALREADY up (a Detach that raced an Attach): told at once.
    display.SuspendLease(&holder, onAttached);
    EXPECT_EQ(attached.load(), 2);
    EXPECT_FALSE(display.Leased());
    // The session ends: no hook after CancelResume.
    display.CancelResume(&holder);
    EXPECT_EQ(display.SuspendedHolders(), 0u);
    EXPECT_EQ(display.Detach(100), ServerWindowDetach::Released);
    display.Attach(FakeWindow(2), 64, 48);
    EXPECT_EQ(attached.load(), 2);
    display.Uninstall(100);
}

// A RESIZE OF THE LEASED WINDOW REACHES ITS HOLDER AT ONCE (surfaceChanged of the same window), and
// WindowExtent answers the platform's latest report - the one size a session publishes. Not for a
// report of the same size, not for anyone but the holder, not after the lease ended. Red with Attach's
// same-window arm not calling the hook (resized stays 0).
TEST(ServerDisplayTest, AResizeOfTheLeasedWindowIsToldToItsHolderAndWindowExtentAnswersIt) {
    FakePlatform platform;
    ServerDisplay display;
    display.Install(HooksFor(platform));
    display.Attach(FakeWindow(0), 64, 48);
    static std::atomic<int> resized{0};
    resized.store(0);
    const auto onResized = [](void*) { resized.fetch_add(1); };
    int holder = 0;
    int stranger = 0;
    ServerWindowLease lease;
    ASSERT_EQ(display.AcquireFor(64, 48, 100, &holder, nullptr, nullptr, nullptr, &lease, onResized),
              ServerWindowAcquire::Acquired);
    void* window = nullptr;
    Uint32 width = 0;
    Uint32 height = 0;
    ASSERT_TRUE(display.WindowExtent(&holder, &window, &width, &height));
    EXPECT_EQ(window, FakeWindow(0));
    EXPECT_EQ(width, 64u);
    EXPECT_EQ(height, 48u);
    EXPECT_FALSE(display.WindowExtent(&stranger, &window, &width, &height)) << "only the holder reads it";

    display.Attach(FakeWindow(0), 64, 60); // the app's bar hidden
    EXPECT_EQ(resized.load(), 1);
    ASSERT_TRUE(display.WindowExtent(&holder, &window, &width, &height));
    EXPECT_EQ(height, 60u);
    display.Attach(FakeWindow(0), 64, 60); // the same report again
    EXPECT_EQ(resized.load(), 1) << "an unchanged extent is not a resize";
    display.Attach(FakeWindow(0), 64, 48); // and back
    EXPECT_EQ(resized.load(), 2);

    display.EndLease(&holder);
    display.Attach(FakeWindow(0), 64, 60);
    EXPECT_EQ(resized.load(), 2) << "a holder whose lease ended is not told";
    EXPECT_FALSE(display.WindowExtent(&holder, &window, &width, &height));
    display.Uninstall(100);
}
