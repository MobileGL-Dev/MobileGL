// MobileGL - MobileGL/MG_Test/Backend/BackendObjectEglStateTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The EGL bookkeeping a backend owns (MG_Backend/EGLSurfaceRegistry.h): the display, the surface
// records, the active surface, each thread's current binding, the capability-probe flag and the
// deferred destroy of a surface that is still current, driven through a test double with no
// device, no EGL driver and no window.
//
// The cases call the public virtuals and the protected building blocks the backends use, and
// observe return values and the hooks, each logged with its thread and the state it saw from
// inside. Only the double's accessors read the base's protected state, so moving the bookkeeping
// into another class re-points those accessors and nothing else. Each case says what it pins; some
// pin quirks, and say so.

#include <gtest/gtest.h>

#include <MG_Backend/BackendObject.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <ostream>
#include <queue>
#include <string>
#include <thread>
#include <utility>

using namespace MobileGL;
using MG_Backend::WindowBackend;
using MG_Backend::WindowHandle;

namespace {
    using namespace std::chrono_literals;

    // A worker step or a latch wait longer than this is a hang: the case aborts instead.
    constexpr auto kStepTimeout = 10s;
    // How long a hook holds the state lock to check that another thread's call does NOT get past
    // it. Only a missing lock lets that call through, so a short window never fails a correct
    // build; at worst a missing lock goes unnoticed on a badly overloaded machine.
    constexpr auto kBlockWindow = 200ms;
    // How long a hook waits for another thread's call that MUST get through while it runs. It
    // stays below kStepTimeout, which bounds the holding call.
    constexpr auto kReachWindow = 5s;

    template <typename T>
    T Fake(std::uintptr_t value) {
        return reinterpret_cast<T>(value);
    }

    const EGLDisplay kDpy = Fake<EGLDisplay>(0xD1);
    const EGLDisplay kDpy2 = Fake<EGLDisplay>(0xD2);
    const EGLSurface kS1 = Fake<EGLSurface>(0x51);
    const EGLSurface kS2 = Fake<EGLSurface>(0x52);
    const EGLSurface kS3 = Fake<EGLSurface>(0x53);
    const EGLSurface kS4 = Fake<EGLSurface>(0x54);
    const EGLSurface kS9 = Fake<EGLSurface>(0x59); // never registered
    const EGLContext kCtx = Fake<EGLContext>(0xC1);
    const EGLContext kCtx2 = Fake<EGLContext>(0xC2);
    const EGLint kIntMin = std::numeric_limits<EGLint>::min();

    WindowHandle Win(std::uintptr_t native, Uint32 width, Uint32 height,
                     WindowBackend backend = WindowBackend::Android) {
        return WindowHandle{
            .Backend = backend, .Handle = reinterpret_cast<void*>(native), .Width = width, .Height = height};
    }

    const WindowHandle kWin1 = Win(0x1001, 640, 480);
    const WindowHandle kWin2 = Win(0x1002, 320, 200);
    const WindowHandle kWin3 = Win(0x1003, 64, 32, WindowBackend::X11);
    const WindowHandle kNoWindow{};

    String Name(EGLSurface surface) {
        if (surface == EGL_NO_SURFACE) return "none";
        if (surface == kS1) return "s1";
        if (surface == kS2) return "s2";
        if (surface == kS3) return "s3";
        if (surface == kS4) return "s4";
        if (surface == kS9) return "s9";
        return "?";
    }

    std::thread::id Self() {
        return std::this_thread::get_id();
    }

    [[noreturn]] void Abort(const char* what) {
        std::fprintf(stderr, "BackendObjectEglStateTest: %s did not finish in time (a hang)\n", what);
        std::fflush(stderr);
        std::abort();
    }

    template <typename T>
    T Await(std::future<T> future, const char* what) {
        if (future.wait_for(kStepTimeout) != std::future_status::ready) Abort(what);
        return future.get();
    }

    class Latch {
    public:
        void Set() {
            {
                const std::lock_guard<std::mutex> lock(m_mutex);
                m_set = true;
            }
            m_cv.notify_all();
        }
        template <typename Duration>
        Bool WaitFor(Duration timeout) {
            std::unique_lock<std::mutex> lock(m_mutex);
            return m_cv.wait_for(lock, timeout, [this] { return m_set; });
        }

    private:
        std::mutex m_mutex;
        std::condition_variable m_cv;
        Bool m_set = false;
    };

    // A long-lived thread that runs posted steps in order. Bindings are keyed by thread id and a
    // finished thread's id can be reused, so a case keeps its workers for its whole length.
    class Worker {
    public:
        Worker() : m_thread([this] { Loop(); }) {}
        ~Worker() {
            {
                const std::lock_guard<std::mutex> lock(m_mutex);
                m_stop = true;
            }
            m_cv.notify_all();
            m_thread.join();
        }
        Worker(const Worker&) = delete;
        Worker& operator=(const Worker&) = delete;

        std::thread::id Id() const { return m_thread.get_id(); }

        template <typename F>
        auto Post(F step) -> std::future<decltype(step())> {
            using Result = decltype(step());
            auto task = std::make_shared<std::packaged_task<Result()>>(std::move(step));
            auto future = task->get_future();
            {
                const std::lock_guard<std::mutex> lock(m_mutex);
                m_steps.push([task] { (*task)(); });
            }
            m_cv.notify_all();
            return future;
        }

        template <typename F>
        auto Run(F step) -> decltype(step()) {
            return Await(Post(std::move(step)), "a worker step");
        }

    private:
        void Loop() {
            for (;;) {
                std::function<void()> step;
                {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    m_cv.wait(lock, [this] { return m_stop || !m_steps.empty(); });
                    if (m_steps.empty()) return;
                    step = std::move(m_steps.front());
                    m_steps.pop();
                }
                step();
            }
        }

        std::mutex m_mutex;
        std::condition_variable m_cv;
        std::queue<std::function<void()>> m_steps;
        Bool m_stop = false;
        std::thread m_thread; // last: it starts running Loop() once the members above exist
    };

    enum class Hook {
        InitWindow,
        InitPbuffer,
        InitCaps,
        Released,
        Forgotten,
        Functions,
        Present,
        SwapInterval
    };

    // The pure virtuals implemented, every hook logged. Present and SetSwapInterval are plain
    // function pointers with no user data, so they reach the live double through a static.
    class EglDouble final : public MG_Backend::BackendObject {
    public:
        using EGLCurrentState = MG_Backend::EGLSurfaceRegistry::CurrentState;
        using EGLSurfaceState = MG_Backend::EGLSurfaceRegistry::SurfaceState;
        using SurfaceKind = MG_Backend::EGLSurfaceRegistry::SurfaceKind;

        // The building blocks the backends drive, under the names the cases call them by.
        // ForgetActiveEGLSurface is called only by the cases that test it.
        Bool ActivateEGLSurface(EGLSurface surface) { return EGLRegistry().Activate(surface); }
        void ForgetActiveEGLSurface() { EGLRegistry().ForgetActiveSurface(); }
        Bool RegisterEGLPbufferSurface(EGLSurface surface, EGLint width, EGLint height) {
            return EGLRegistry().RegisterPbufferSurface(surface, width, height);
        }
        Bool RegisterEGLWindowSurface(EGLSurface surface, const WindowHandle& window) {
            return EGLRegistry().RegisterWindowSurface(surface, window);
        }
        void ResetEGLRuntimeState() { EGLRegistry().ResetRuntimeState(); }

        // The observable state as a hook saw it.
        struct View {
            Bool HasActive = false;
            EGLSurface Active = EGL_NO_SURFACE;
            SurfaceKind Kind = SurfaceKind::None;
            EGLSurface Activating = EGL_NO_SURFACE;
            WindowHandle Window{};
            Bool CapsProbed = false;
            Optional<EGLSurfaceState> Subject; // the record of the surface the hook is about
            Optional<EGLCurrentState> Current; // the calling thread's binding
        };

        struct Call {
            Hook What = Hook::InitCaps;
            std::thread::id Thread;
            EGLSurface Surface = EGL_NO_SURFACE; // the activating surface for the init hooks
            EGLint Width = 0;
            EGLint Height = 0;
            Int Interval = 0;
            Bool HasState = false;
            View State;
        };

        enum class PbufferInit {
            Succeed,
            Fail,
            Base
        };

        EglDouble() {
            m_functions.Present = &PresentThunk;
            m_functions.SetSwapInterval = &SwapIntervalThunk;
            s_live = this;
        }
        ~EglDouble() override {
            EglDouble* self = this;
            s_live.compare_exchange_strong(self, nullptr);
        }

        Uint64 HookCount() const { return m_hookCalls.load(); }
        Uint64 PresentCount() const { return m_presents.load(); }
        Uint64 SwapIntervalCount() const { return m_swapIntervals.load(); }
        // Present or SetSwapInterval calls that found no live double, as during its destruction.
        static Uint64 OrphanCalls() { return s_orphanCalls.load(); }
        static void ResetOrphanCalls() { s_orphanCalls = 0; }

        // ---- switches ----
        Bool failWindow = false;
        Bool failCaps = false;
        PbufferInit pbufferInit = PbufferInit::Succeed;
        std::function<void(const Call&)> onHook; // runs inside every hook, after it is logged
        std::function<void()> onSwapOverride;    // runs when the virtual SwapEGLBuffers is entered
        std::atomic<Int> swapOverrideCalls{0};
        void SetPresentNull(Bool null) { m_functions.Present = null ? nullptr : &PresentThunk; }
        void SetSwapIntervalNull(Bool null) { m_functions.SetSwapInterval = null ? nullptr : &SwapIntervalThunk; }

        // ---- the hook log ----
        Vector<Call> Calls() const {
            const std::lock_guard<std::mutex> lock(m_logMutex);
            return m_calls;
        }
        void ClearCalls() {
            const std::lock_guard<std::mutex> lock(m_logMutex);
            m_calls.clear();
        }

        // ---- the only readers (and the one writer) of the base's EGL state ----
        // Guard() must take the same recursive mutex the base holds around every hook, because the
        // hooks take their state snapshots through these accessors from inside it.
        Bool HasActiveSurface() const {
            const auto lock = Guard();
            return EGLRegistry().HasActiveSurface();
        }
        EGLSurface ActiveSurface() const {
            const auto lock = Guard();
            return EGLRegistry().ActiveSurface();
        }
        SurfaceKind ActiveKind() const {
            const auto lock = Guard();
            return EGLRegistry().ActiveSurfaceKind();
        }
        EGLSurface ActivatingSurface() const {
            const auto lock = Guard();
            return EGLRegistry().ActivatingSurface();
        }
        WindowHandle WindowHandleNow() const {
            const auto lock = Guard();
            return EGLRegistry().CurrentWindowHandle();
        }
        Bool CapsProbed() const {
            const auto lock = Guard();
            return EGLRegistry().CapabilitiesInitialized();
        }
        Bool DisplayInitialized() const {
            const auto lock = Guard();
            return EGLRegistry().DisplayInitialized();
        }
        EGLDisplay DisplayNow() const {
            const auto lock = Guard();
            return EGLRegistry().Display();
        }
        Optional<EGLSurfaceState> Record(EGLSurface surface) const {
            const auto lock = Guard();
            return EGLRegistry().Find(surface);
        }
        // The base lookup with no lock of the double's own, so a probe of it meets only the
        // lookup's own locking.
        Bool IsRegistered(EGLSurface surface) const { return EGLRegistry().IsRegistered(surface); }
        Optional<EGLCurrentState> CurrentHere() const {
            const auto lock = Guard();
            return EGLRegistry().CurrentOn(std::this_thread::get_id());
        }
        SizeT CurrentCount() const {
            const auto lock = Guard();
            return EGLRegistry().CurrentThreadCount();
        }
        // A mark in the format-capability cache, which the EGL bookkeeping never touches.
        void MarkFormatCapabilities() { MutableFormatCapabilities().SampleCounts[0][0] = {4}; }
        Bool FormatCapabilitiesMarked() const { return GetFormatCapabilities().SampleCounts[0][0] == Vector<Int>{4}; }

        // ---- the pure virtuals ----
        void Initialize() override {}
        Bool InitCapabilities() override {
            Note({.What = Hook::InitCaps}, true);
            return !failCaps;
        }
        Bool InitWindowSurface() override {
            Note({.What = Hook::InitWindow, .Surface = ActivatingSurface()}, true);
            return !failWindow;
        }
        const RendererInfo& GetRendererInfo() const override { return m_info; }
        String GetBackendAPIVersionString() const override { return "test"; }
        const MG_Backend::GlobalBackendFunctionsTable& GetBackendFunctions() const override {
            Note({.What = Hook::Functions}, false);
            return m_functions;
        }
        const MG_Backend::DynamicBackendParameters& GetDynamicParameters() const override { return m_params; }
        BackendType GetBackendType() const override { return BackendType::Unknown; }

        // Forwards to the base, counting: the default swap-with-damage must dispatch here.
        Bool SwapEGLBuffers(EGLDisplay dpy, EGLSurface draw) override {
            ++swapOverrideCalls;
            if (onSwapOverride) onSwapOverride();
            return BackendObject::SwapEGLBuffers(dpy, draw);
        }

    private:
        Bool InitPbufferSurface(EGLint width, EGLint height) override {
            Note({.What = Hook::InitPbuffer, .Surface = ActivatingSurface(), .Width = width, .Height = height}, true);
            switch (pbufferInit) {
            case PbufferInit::Succeed:
                return true;
            case PbufferInit::Fail:
                return false;
            case PbufferInit::Base:
                return BackendObject::InitPbufferSurface(width, height);
            }
            return false;
        }
        void OnEGLSurfaceReleased(EGLSurface surface) override {
            Note({.What = Hook::Released, .Surface = surface}, true);
        }
        void OnEGLSurfaceForgotten(EGLSurface surface) override {
            Note({.What = Hook::Forgotten, .Surface = surface}, true);
        }

        std::unique_lock<std::recursive_mutex> Guard() const { return EGLRegistry().Lock(); }

        // The state view locks, so the calls that run without the lock (the function table read
        // and SetSwapInterval) are logged without one.
        void Note(Call call, Bool withState) const {
            ++m_hookCalls;
            call.Thread = std::this_thread::get_id();
            if (withState) {
                const EGLSurface subject = call.What == Hook::Present ? ActiveSurface() : call.Surface;
                call.HasState = true;
                call.State = View{.HasActive = HasActiveSurface(),
                                  .Active = ActiveSurface(),
                                  .Kind = ActiveKind(),
                                  .Activating = ActivatingSurface(),
                                  .Window = WindowHandleNow(),
                                  .CapsProbed = CapsProbed(),
                                  .Subject = Record(subject),
                                  .Current = CurrentHere()};
            }
            {
                const std::lock_guard<std::mutex> lock(m_logMutex);
                m_calls.push_back(call);
            }
            if (onHook) onHook(call);
        }

        static void PresentThunk() {
            EglDouble* self = s_live.load();
            if (!self) {
                ++s_orphanCalls;
                return;
            }
            ++self->m_presents;
            self->Note({.What = Hook::Present}, true);
        }
        static void SwapIntervalThunk(Int interval) {
            EglDouble* self = s_live.load();
            if (!self) {
                ++s_orphanCalls;
                return;
            }
            ++self->m_swapIntervals;
            self->Note({.What = Hook::SwapInterval, .Interval = interval}, false);
        }

        static inline std::atomic<EglDouble*> s_live{nullptr};
        static inline std::atomic<Uint64> s_orphanCalls{0};

        MG_Backend::GlobalBackendFunctionsTable m_functions{};
        MG_Backend::DynamicBackendParameters m_params{};
        RendererInfo m_info{};
        mutable std::atomic<Uint64> m_hookCalls{0};
        std::atomic<Uint64> m_presents{0};
        std::atomic<Uint64> m_swapIntervals{0};
        mutable std::mutex m_logMutex;
        mutable Vector<Call> m_calls;
    };

    using Kind = EglDouble::SurfaceKind;
    using Rec = EglDouble::EGLSurfaceState;
    using Entry = EglDouble::EGLCurrentState;

    String Describe(const EglDouble::Call& call) {
        switch (call.What) {
        case Hook::InitWindow:
            return "InitWindow(" + Name(call.Surface) + ")";
        case Hook::InitPbuffer:
            return "InitPbuffer(" + Name(call.Surface) + "," + std::to_string(call.Width) + "," +
                   std::to_string(call.Height) + ")";
        case Hook::InitCaps:
            return "InitCaps";
        case Hook::Released:
            return "Released(" + Name(call.Surface) + ")";
        case Hook::Forgotten:
            return "Forgotten(" + Name(call.Surface) + ")";
        case Hook::Functions:
            return "Functions";
        case Hook::Present:
            return "Present";
        case Hook::SwapInterval:
            return "SwapInterval(" + std::to_string(call.Interval) + ")";
        }
        return "?";
    }

    String DescribeWindow(const WindowHandle& window) {
        return "{backend " + std::to_string(static_cast<int>(window.Backend)) + ", handle " +
               std::to_string(reinterpret_cast<std::uintptr_t>(window.Handle)) + ", " + std::to_string(window.Width) +
               "x" + std::to_string(window.Height) + "}";
    }

    ::testing::AssertionResult WindowIs(const WindowHandle& actual, const WindowHandle& expected) {
        if (actual.Backend == expected.Backend && actual.Handle == expected.Handle && actual.Width == expected.Width &&
            actual.Height == expected.Height)
            return ::testing::AssertionSuccess();
        return ::testing::AssertionFailure()
               << "window " << DescribeWindow(actual) << ", expected " << DescribeWindow(expected);
    }

    ::testing::AssertionResult RecordIs(const Optional<Rec>& actual, const Rec& expected) {
        if (!actual) return ::testing::AssertionFailure() << "not registered";
        if (actual->Kind != expected.Kind) return ::testing::AssertionFailure() << "kind differs";
        if (actual->DestroyPending != expected.DestroyPending)
            return ::testing::AssertionFailure() << "DestroyPending is " << actual->DestroyPending;
        if (actual->Activated != expected.Activated)
            return ::testing::AssertionFailure() << "Activated is " << actual->Activated;
        if (!WindowIs(actual->Window, expected.Window))
            return ::testing::AssertionFailure() << "record " << WindowIs(actual->Window, expected.Window).message();
        if (actual->Width != expected.Width || actual->Height != expected.Height)
            return ::testing::AssertionFailure() << "extent " << actual->Width << "x" << actual->Height << ", expected "
                                                 << expected.Width << "x" << expected.Height;
        return ::testing::AssertionSuccess();
    }

    // A binding as the base writes it: draw == read == `surface`.
    ::testing::AssertionResult BoundTo(const Optional<Entry>& entry, EGLDisplay dpy, EGLSurface surface,
                                       EGLContext ctx) {
        if (!entry) return ::testing::AssertionFailure() << "no binding";
        if (entry->Display != dpy || entry->DrawSurface != surface || entry->ReadSurface != surface ||
            entry->Context != ctx)
            return ::testing::AssertionFailure()
                   << "binding draw " << Name(entry->DrawSurface) << " read " << Name(entry->ReadSurface)
                   << " (display/context " << (entry->Display == dpy) << "/" << (entry->Context == ctx) << ")";
        return ::testing::AssertionSuccess();
    }

    // A surface is active exactly when the active handle is set, and none means kind None.
    ::testing::AssertionResult ActiveInvariantHolds(const EglDouble& backend) {
        if (backend.HasActiveSurface() != (backend.ActiveSurface() != EGL_NO_SURFACE))
            return ::testing::AssertionFailure() << "active flag and active surface disagree";
        if (!backend.HasActiveSurface() && backend.ActiveKind() != Kind::None)
            return ::testing::AssertionFailure() << "no active surface but a kind";
        return ::testing::AssertionSuccess();
    }

    // Records are built field by field, so the expectations do not depend on the field order.
    Rec WindowRecord(const WindowHandle& window, EGLint width, EGLint height, Bool activated = false,
                     Bool pending = false) {
        Rec record;
        record.Kind = Kind::Window;
        record.DestroyPending = pending;
        record.Activated = activated;
        record.Window = window;
        record.Width = width;
        record.Height = height;
        return record;
    }

    Rec PbufferRecord(EGLint width, EGLint height, Bool activated = false, Bool pending = false,
                      const WindowHandle& window = WindowHandle{}) {
        Rec record = WindowRecord(window, width, height, activated, pending);
        record.Kind = Kind::Pbuffer;
        return record;
    }

    enum class ProbeOutcome {
        Blocked,
        GotThrough,
        HookNotReached
    };

    std::ostream& operator<<(std::ostream& out, ProbeOutcome outcome) {
        switch (outcome) {
        case ProbeOutcome::Blocked:
            return out << "blocked";
        case ProbeOutcome::GotThrough:
            return out << "got through";
        case ProbeOutcome::HookNotReached:
            return out << "the held hook was never reached";
        }
        return out << "?";
    }

    class EglCase : public ::testing::Test {
    protected:
        void SetUp() override {
            EglDouble::ResetOrphanCalls();
            b = MakeUnique<EglDouble>();
        }
        // A new double. The old one goes first, so the live-double static never names a dead one.
        void Fresh() {
            b.reset();
            b = MakeUnique<EglDouble>();
        }

        // The logged calls without the function-table reads, a getter with no effect: only the
        // cases about when the table is read look at those.
        Vector<EglDouble::Call> Hooks() const {
            Vector<EglDouble::Call> hooks;
            for (const auto& call : b->Calls())
                if (call.What != Hook::Functions) hooks.push_back(call);
            return hooks;
        }
        Vector<String> Trace() const {
            Vector<String> trace;
            for (const auto& call : Hooks())
                trace.push_back(Describe(call));
            return trace;
        }
        Vector<String> TraceWithTableReads() const {
            Vector<String> trace;
            for (const auto& call : b->Calls())
                trace.push_back(Describe(call));
            return trace;
        }
        // Every logged call, table reads included, ran on `thread`.
        ::testing::AssertionResult AllCallsOn(std::thread::id thread) const {
            for (const auto& call : b->Calls())
                if (call.Thread != thread)
                    return ::testing::AssertionFailure() << Describe(call) << " ran on another thread";
            return ::testing::AssertionSuccess();
        }

        Bool Init() { return b->InitializeEGLDisplay(kDpy, nullptr, nullptr); }
        // `surface` created as a window and made current on the calling thread.
        Bool BoundWindow(EGLSurface surface, const WindowHandle& window, EGLContext ctx = kCtx) {
            return b->CreateEGLWindowSurface(surface, window) && b->MakeEGLCurrent(kDpy, surface, surface, ctx);
        }
        Bool Bind(EGLSurface surface, EGLContext ctx = kCtx) { return b->MakeEGLCurrent(kDpy, surface, surface, ctx); }
        Bool Release() { return b->MakeEGLCurrent(EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); }
        Bool Swap(EGLSurface surface) { return b->SwapEGLBuffers(kDpy, surface); }
        // The same three on `worker`'s thread.
        Bool BindOn(Worker& worker, EGLSurface surface, EGLContext ctx = kCtx) {
            return worker.Run([this, surface, ctx] { return Bind(surface, ctx); });
        }
        Bool ReleaseOn(Worker& worker) {
            return worker.Run([this] { return Release(); });
        }
        Bool SwapOn(Worker& worker, EGLSurface surface) {
            return worker.Run([this, surface] { return Swap(surface); });
        }
        Optional<Entry> CurrentOn(Worker& worker) {
            return worker.Run([this] { return b->CurrentHere(); });
        }
        Bool NoKnownRecord() const {
            for (EGLSurface surface : {EGL_NO_SURFACE, kS1, kS2, kS3, kS4, kS9})
                if (b->Record(surface)) return false;
            return true;
        }

        UniquePtr<EglDouble> b;
    };

    class BackendObjectEglInitial : public EglCase {};
    class BackendObjectEglDisplay : public EglCase {};
    class BackendObjectEglRegister : public EglCase {};
    class BackendObjectEglActivate : public EglCase {};
    class BackendObjectEglCreate : public EglCase {};
    class BackendObjectEglResize : public EglCase {};
    class BackendObjectEglMakeCurrent : public EglCase {};
    class BackendObjectEglCaps : public EglCase {};
    class BackendObjectEglSwap : public EglCase {};
    class BackendObjectEglSwapInterval : public EglCase {};
    class BackendObjectEglRelease : public EglCase {};
    class BackendObjectEglThreads : public EglCase {};
    class BackendObjectEglReset : public EglCase {};
    class BackendObjectEglWindowHandle : public EglCase {};

    class BackendObjectEglLocking : public EglCase {
    protected:
        // Starts `hold` on `holder`; once `holder` is inside its first `held` hook, runs `probe` on
        // `other`. The hook waits up to `window` for `probe` to set the latch (set for it once it
        // returns), and the outcome says whether it did while the hook was still running. A
        // holding call that returns without reaching the hook is reported, not waited out.
        ProbeOutcome ProbeDuringHook(Hook held, std::function<void()> hold, std::function<void(Latch&)> probe,
                                     std::chrono::milliseconds window) {
            Latch inHook;
            Latch probeStarting;
            Latch reached;
            std::atomic<Bool> armed{true};
            std::atomic<Bool> gotThrough{false};
            const std::thread::id holderId = holder.Id();
            b->onHook = [&](const EglDouble::Call& call) {
                if (call.What != held || Self() != holderId || !armed.exchange(false)) return;
                inHook.Set();
                if (!probeStarting.WaitFor(kStepTimeout)) return;
                gotThrough = reached.WaitFor(window);
            };
            auto holding = holder.Post(std::move(hold));
            const auto deadline = std::chrono::steady_clock::now() + kStepTimeout;
            while (!inHook.WaitFor(10ms)) {
                if (holding.wait_for(0s) == std::future_status::ready) {
                    b->onHook = nullptr;
                    return ProbeOutcome::HookNotReached;
                }
                if (std::chrono::steady_clock::now() > deadline) Abort("reaching the held hook");
            }
            auto probing = other.Post([&] {
                probeStarting.Set();
                probe(reached);
                reached.Set();
            });
            Await(std::move(holding), "the holding call");
            Await(std::move(probing), "the probing call");
            b->onHook = nullptr;
            return gotThrough ? ProbeOutcome::GotThrough : ProbeOutcome::Blocked;
        }

        Worker holder;
        Worker other;
    };
} // namespace

// ================================ initial state, hook-free methods ================================

TEST_F(BackendObjectEglInitial, AFreshBackendHoldsNothing) {
    EXPECT_FALSE(b->DisplayInitialized());
    EXPECT_EQ(b->DisplayNow(), EGL_NO_DISPLAY);
    EXPECT_TRUE(NoKnownRecord());
    EXPECT_EQ(b->CurrentCount(), 0u);
    EXPECT_FALSE(b->HasActiveSurface());
    EXPECT_EQ(b->ActiveSurface(), EGL_NO_SURFACE);
    EXPECT_EQ(b->ActiveKind(), Kind::None);
    EXPECT_EQ(b->ActivatingSurface(), EGL_NO_SURFACE);
    EXPECT_FALSE(b->CapsProbed());
    EXPECT_TRUE(WindowIs(b->WindowHandleNow(), kNoWindow));
    EXPECT_TRUE(ActiveInvariantHolds(*b));
    EXPECT_TRUE(TraceWithTableReads().empty());
    EXPECT_EQ(b->HookCount(), 0u);
}

// Destroying a backend with an active, a pending and an idle surface and two threads'
// bindings calls no pure virtual (that aborts) and reaches nothing of the double. What the
// destructor might do with the base's own default hooks cannot be seen from a derived class.
TEST_F(BackendObjectEglInitial, DestructionRunsNoHook) {
    Worker other;
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    ASSERT_TRUE(BindOn(other, kS1, kCtx2));
    ASSERT_TRUE(b->CreateEGLPbufferSurface(kS2, 8, 8));
    ASSERT_TRUE(b->RegisterEGLWindowSurface(kS3, kWin3));
    b->ReleaseEGLSurface(kS1);
    b.reset();
    EXPECT_EQ(EglDouble::OrphanCalls(), 0u);
}

// The bookkeeping-only methods run no hook and never read the function table.
TEST_F(BackendObjectEglInitial, BookkeepingOnlyMethodsRunNoHook) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    ASSERT_TRUE(b->RegisterEGLPbufferSurface(kS2, 4, 4));
    b->ClearCalls();
    EXPECT_TRUE(b->InitializeEGLDisplay(kDpy, nullptr, nullptr));
    EXPECT_TRUE(b->RegisterEGLWindowSurface(kS3, kWin3));
    EXPECT_TRUE(b->RegisterEGLPbufferSurface(kS2, 5, 5));
    EXPECT_TRUE(b->IsRegistered(kS3));
    EXPECT_TRUE(b->ResizeEGLWindowSurface(kS1, 10, 10));
    b->SetWindowHandle(kWin2);
    b->ForgetActiveEGLSurface();
    b->ResetEGLRuntimeState();
    b->ReleaseEGLResources();
    EXPECT_TRUE(TraceWithTableReads().empty());
}

// ============================================ display ============================================

TEST_F(BackendObjectEglDisplay, NoDisplayIsRefusedAndWritesNothing) {
    EGLint major = -7;
    EGLint minor = -7;
    EXPECT_FALSE(b->InitializeEGLDisplay(EGL_NO_DISPLAY, &major, &minor));
    EXPECT_EQ(major, -7);
    EXPECT_EQ(minor, -7);
    EXPECT_FALSE(b->DisplayInitialized());
    EXPECT_EQ(b->DisplayNow(), EGL_NO_DISPLAY);
    // Nor does it undo an earlier display.
    ASSERT_TRUE(Init());
    EXPECT_FALSE(b->InitializeEGLDisplay(EGL_NO_DISPLAY, &major, &minor));
    EXPECT_EQ(major, -7);
    EXPECT_EQ(minor, -7);
    EXPECT_TRUE(b->DisplayInitialized());
    EXPECT_EQ(b->DisplayNow(), kDpy);
    EXPECT_TRUE(Trace().empty());
}

// A refused display leaves the bookkeeping that already exists alone: the records (a pending
// one too), every thread's binding, the active surface, the window handle and the probe flag.
TEST_F(BackendObjectEglDisplay, ARefusedDisplayLeavesTheBookkeepingAlone) {
    Worker other;
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    ASSERT_TRUE(BindOn(other, kS1, kCtx2));
    ASSERT_TRUE(b->RegisterEGLPbufferSurface(kS2, 4, 4));
    b->ReleaseEGLSurface(kS1);
    b->ClearCalls();
    EXPECT_FALSE(b->InitializeEGLDisplay(EGL_NO_DISPLAY, nullptr, nullptr));
    EXPECT_TRUE(Trace().empty());
    EXPECT_TRUE(RecordIs(b->Record(kS1), WindowRecord(kWin1, 640, 480, true, true)));
    EXPECT_TRUE(RecordIs(b->Record(kS2), PbufferRecord(4, 4)));
    EXPECT_TRUE(BoundTo(b->CurrentHere(), kDpy, kS1, kCtx));
    EXPECT_TRUE(BoundTo(CurrentOn(other), kDpy, kS1, kCtx2));
    EXPECT_EQ(b->ActiveSurface(), kS1);
    EXPECT_EQ(b->ActiveKind(), Kind::Window);
    EXPECT_TRUE(WindowIs(b->WindowHandleNow(), kWin1));
    EXPECT_TRUE(b->CapsProbed());
    EXPECT_TRUE(b->DisplayInitialized());
    EXPECT_EQ(b->DisplayNow(), kDpy);
}

TEST_F(BackendObjectEglDisplay, ADisplayIsAdoptedAsVersionOnePointFive) {
    EGLint major = 0;
    EGLint minor = 0;
    EXPECT_TRUE(b->InitializeEGLDisplay(kDpy, &major, &minor));
    EXPECT_EQ(major, 1);
    EXPECT_EQ(minor, 5);
    EXPECT_TRUE(b->DisplayInitialized());
    EXPECT_EQ(b->DisplayNow(), kDpy);
    EXPECT_TRUE(Trace().empty());
}

TEST_F(BackendObjectEglDisplay, NullVersionPointersAreTolerated) {
    EXPECT_TRUE(b->InitializeEGLDisplay(kDpy, nullptr, nullptr));
    EGLint minor = 0;
    EXPECT_TRUE(b->InitializeEGLDisplay(kDpy, nullptr, &minor));
    EXPECT_EQ(minor, 5);
    EGLint major = 0;
    EXPECT_TRUE(b->InitializeEGLDisplay(kDpy, &major, nullptr));
    EXPECT_EQ(major, 1);
}

// A second display replaces the first and nothing else; the first is then refused.
TEST_F(BackendObjectEglDisplay, ASecondDisplayReplacesTheFirst) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    b->ClearCalls();
    EXPECT_TRUE(b->InitializeEGLDisplay(kDpy2, nullptr, nullptr));
    EXPECT_TRUE(b->DisplayInitialized());
    EXPECT_EQ(b->DisplayNow(), kDpy2);
    EXPECT_EQ(b->ActiveSurface(), kS1);
    EXPECT_TRUE(b->CapsProbed());
    EXPECT_TRUE(RecordIs(b->Record(kS1), WindowRecord(kWin1, 640, 480, true)));
    EXPECT_TRUE(BoundTo(b->CurrentHere(), kDpy, kS1, kCtx));
    EXPECT_FALSE(b->MakeEGLCurrent(kDpy, kS1, kS1, kCtx));
    EXPECT_FALSE(b->SwapEGLBuffers(kDpy, kS1));
    EXPECT_TRUE(Trace().empty());
    // The new display works with the surfaces registered under the old one.
    EXPECT_TRUE(b->MakeEGLCurrent(kDpy2, kS1, kS1, kCtx));
    EXPECT_TRUE(BoundTo(b->CurrentHere(), kDpy2, kS1, kCtx));
    EXPECT_TRUE(b->SwapEGLBuffers(kDpy2, kS1));
    EXPECT_EQ(Trace(), (Vector<String>{"Present"}));
}

TEST_F(BackendObjectEglDisplay, ReinitializingTheSameDisplayChangesNothing) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    b->ClearCalls();
    EGLint major = 0;
    EGLint minor = 0;
    EXPECT_TRUE(b->InitializeEGLDisplay(kDpy, &major, &minor));
    EXPECT_EQ(major, 1);
    EXPECT_EQ(minor, 5);
    EXPECT_EQ(b->DisplayNow(), kDpy);
    EXPECT_EQ(b->ActiveSurface(), kS1);
    EXPECT_TRUE(b->CapsProbed());
    EXPECT_TRUE(BoundTo(b->CurrentHere(), kDpy, kS1, kCtx));
    EXPECT_TRUE(RecordIs(b->Record(kS1), WindowRecord(kWin1, 640, 480, true)));
    EXPECT_TRUE(Swap(kS1));
    EXPECT_EQ(Trace(), (Vector<String>{"Present"}));
}

// ========================================= registration =========================================

// A window needs a display, a surface handle and a native window. Only Unknown is refused as
// a backend: a value outside the enumeration is accepted (a quirk).
TEST_F(BackendObjectEglRegister, AWindowNeedsADisplayASurfaceAndANativeWindow) {
    EXPECT_FALSE(b->RegisterEGLWindowSurface(kS1, kWin1));
    ASSERT_TRUE(Init());
    EXPECT_FALSE(b->RegisterEGLWindowSurface(EGL_NO_SURFACE, kWin1));
    EXPECT_FALSE(b->RegisterEGLWindowSurface(kS1, Win(0x1001, 8, 8, WindowBackend::Unknown)));
    EXPECT_FALSE(b->RegisterEGLWindowSurface(kS1, Win(0, 8, 8)));
    EXPECT_TRUE(NoKnownRecord());
    EXPECT_TRUE(Trace().empty());
    EXPECT_TRUE(b->RegisterEGLWindowSurface(kS1, Win(0x1001, 8, 8, WindowBackend::X11)));
    EXPECT_TRUE(b->RegisterEGLWindowSurface(kS2, Win(0x1002, 8, 8, WindowBackend::MetalLayer)));
    EXPECT_TRUE(b->RegisterEGLWindowSurface(kS3, Win(0x1003, 8, 8, WindowBackend::Win32)));
    EXPECT_TRUE(b->RegisterEGLWindowSurface(kS4, Win(0x1004, 8, 8, WindowBackend::WindowBackendCount)));
    EXPECT_TRUE(Trace().empty());
}

// The record keeps the handle verbatim and clamps its own extent to at least 1x1.
TEST_F(BackendObjectEglRegister, AWindowRecordKeepsTheHandleAndClampsItsExtent) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(b->RegisterEGLWindowSurface(kS1, Win(0x1001, 0, 0)));
    EXPECT_TRUE(RecordIs(b->Record(kS1), WindowRecord(Win(0x1001, 0, 0), 1, 1)));
    ASSERT_TRUE(b->RegisterEGLWindowSurface(kS2, Win(0x1002, 640, 0)));
    EXPECT_TRUE(RecordIs(b->Record(kS2), WindowRecord(Win(0x1002, 640, 0), 640, 1)));
    ASSERT_TRUE(b->RegisterEGLWindowSurface(kS3, Win(0x1003, 0, 480)));
    EXPECT_TRUE(RecordIs(b->Record(kS3), WindowRecord(Win(0x1003, 0, 480), 1, 480)));
    EXPECT_FALSE(b->HasActiveSurface());
    EXPECT_EQ(b->ActivatingSurface(), EGL_NO_SURFACE);
    EXPECT_TRUE(WindowIs(b->WindowHandleNow(), kNoWindow));
    EXPECT_TRUE(Trace().empty());
}

// A quirk: an extent above INT_MAX is not clamped; it wraps to a negative EGLint.
TEST_F(BackendObjectEglRegister, AnExtentAboveIntMaxWrapsNegative) {
    ASSERT_TRUE(Init());
    const WindowHandle huge = Win(0x1001, 0x80000000u, 0xFFFFFFFFu);
    ASSERT_TRUE(b->RegisterEGLWindowSurface(kS1, huge));
    EXPECT_TRUE(RecordIs(b->Record(kS1), WindowRecord(huge, kIntMin, -1)));
}

// Registering beside an active surface leaves it and the window handle alone.
TEST_F(BackendObjectEglRegister, RegisteringDoesNotActivate) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(b->CreateEGLWindowSurface(kS1, kWin1));
    b->ClearCalls();
    ASSERT_TRUE(b->RegisterEGLWindowSurface(kS2, kWin2));
    EXPECT_TRUE(RecordIs(b->Record(kS2), WindowRecord(kWin2, 320, 200)));
    EXPECT_EQ(b->ActiveSurface(), kS1);
    EXPECT_EQ(b->ActivatingSurface(), kS1);
    EXPECT_TRUE(WindowIs(b->WindowHandleNow(), kWin1));
    EXPECT_TRUE(Trace().empty());
}

TEST_F(BackendObjectEglRegister, APbufferNeedsADisplayASurfaceAndAPositiveExtent) {
    EXPECT_FALSE(b->RegisterEGLPbufferSurface(kS1, 8, 8));
    ASSERT_TRUE(Init());
    EXPECT_FALSE(b->RegisterEGLPbufferSurface(EGL_NO_SURFACE, 8, 8));
    EXPECT_FALSE(b->RegisterEGLPbufferSurface(kS1, 0, 8));
    EXPECT_FALSE(b->RegisterEGLPbufferSurface(kS1, 8, 0));
    EXPECT_FALSE(b->RegisterEGLPbufferSurface(kS1, -1, 8));
    EXPECT_FALSE(b->RegisterEGLPbufferSurface(kS1, 8, -1));
    EXPECT_TRUE(NoKnownRecord());
    ASSERT_TRUE(b->RegisterEGLPbufferSurface(kS1, 1, 1));
    EXPECT_TRUE(RecordIs(b->Record(kS1), PbufferRecord(1, 1)));
    ASSERT_TRUE(b->RegisterEGLPbufferSurface(kS2, 30, 40));
    EXPECT_TRUE(RecordIs(b->Record(kS2), PbufferRecord(30, 40)));
    EXPECT_FALSE(b->HasActiveSurface());
    EXPECT_TRUE(Trace().empty());
}

// Re-registering replaces the whole record. A pending destroy is canceled and
// Activated is reset (the next real activation re-probes); the active view is left as it was.
TEST_F(BackendObjectEglRegister, ReRegisteringReplacesTheWholeRecord) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    b->ReleaseEGLSurface(kS1);
    ASSERT_TRUE(RecordIs(b->Record(kS1), WindowRecord(kWin1, 640, 480, true, true)));
    b->ClearCalls();
    ASSERT_TRUE(b->RegisterEGLPbufferSurface(kS1, 30, 40));
    EXPECT_TRUE(RecordIs(b->Record(kS1), PbufferRecord(30, 40)));
    EXPECT_EQ(b->ActiveSurface(), kS1);
    EXPECT_EQ(b->ActiveKind(), Kind::Window); // not refreshed from the new record
    EXPECT_TRUE(WindowIs(b->WindowHandleNow(), kWin1));
    EXPECT_TRUE(b->CapsProbed());
    EXPECT_TRUE(Trace().empty());
    // The canceled destroy does not complete when the binding goes.
    EXPECT_TRUE(Release());
    EXPECT_TRUE(b->IsRegistered(kS1));
    EXPECT_TRUE(Trace().empty());
    // The replaced record's next real activation clears the probe flag again.
    ASSERT_TRUE(BoundWindow(kS2, kWin2));
    b->ClearCalls();
    EXPECT_TRUE(Bind(kS1));
    EXPECT_EQ(Trace(), (Vector<String>{"InitPbuffer(s1,30,40)", "InitCaps"}));
    EXPECT_EQ(b->ActiveKind(), Kind::Pbuffer);
    EXPECT_TRUE(ActiveInvariantHolds(*b));
}

// Re-registering a pending window as a window cancels its destroy and resets Activated too,
// through RegisterEGLWindowSurface and through the base create alike, with no hook.
TEST_F(BackendObjectEglRegister, ReRegisteringAPendingWindowCancelsItsDestroy) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    b->ReleaseEGLSurface(kS1);
    b->ClearCalls();
    ASSERT_TRUE(b->RegisterEGLWindowSurface(kS1, kWin3));
    EXPECT_TRUE(RecordIs(b->Record(kS1), WindowRecord(kWin3, 64, 32)));
    EXPECT_TRUE(Release());
    EXPECT_TRUE(b->IsRegistered(kS1));
    EXPECT_TRUE(Trace().empty());
    // The same through the create, whose activation short-circuits on the still active s1.
    ASSERT_TRUE(Bind(kS1));
    b->ReleaseEGLSurface(kS1);
    ASSERT_TRUE(RecordIs(b->Record(kS1), WindowRecord(kWin3, 64, 32, false, true)));
    EXPECT_TRUE(b->CreateEGLWindowSurface(kS1, kWin2));
    EXPECT_TRUE(RecordIs(b->Record(kS1), WindowRecord(kWin2, 320, 200)));
    EXPECT_TRUE(Release());
    EXPECT_TRUE(b->IsRegistered(kS1));
    EXPECT_EQ(b->ActiveSurface(), kS1);
    EXPECT_TRUE(Trace().empty());
}

// A lookup finds the stored record and never inserts one.
TEST_F(BackendObjectEglRegister, ALookupNeverInserts) {
    ASSERT_TRUE(Init());
    EXPECT_FALSE(b->IsRegistered(kS9));
    EXPECT_FALSE(b->Record(kS9).has_value());
    ASSERT_TRUE(b->RegisterEGLPbufferSurface(kS1, 2, 3));
    EXPECT_TRUE(b->IsRegistered(kS1));
    EXPECT_TRUE(RecordIs(b->Record(kS1), PbufferRecord(2, 3)));
    EXPECT_FALSE(b->IsRegistered(kS9));
    EXPECT_FALSE(b->Record(kS9).has_value());
}

// ========================================== activation ==========================================

TEST_F(BackendObjectEglActivate, AnUnregisteredSurfaceIsRefusedWithoutAWrite) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    b->ClearCalls();
    EXPECT_FALSE(b->ActivateEGLSurface(kS9));
    EXPECT_EQ(b->ActivatingSurface(), kS1);
    EXPECT_EQ(b->ActiveSurface(), kS1);
    EXPECT_TRUE(WindowIs(b->WindowHandleNow(), kWin1));
    EXPECT_TRUE(b->CapsProbed());
    EXPECT_FALSE(b->Record(kS9).has_value());
    EXPECT_TRUE(Trace().empty());
}

// The active surface short-circuits: no hook and no write of the activating surface,
// Activated, the probe flag or the window handle.
TEST_F(BackendObjectEglActivate, TheActiveSurfaceShortCircuits) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    ASSERT_TRUE(b->RegisterEGLWindowSurface(kS2, kWin2));
    b->failWindow = true;
    ASSERT_FALSE(b->ActivateEGLSurface(kS2)); // leaves the activating surface on s2
    b->failWindow = false;
    ASSERT_TRUE(b->RegisterEGLWindowSurface(kS1, kWin3)); // Activated back to false
    b->SetWindowHandle(Win(0x1004, 1, 2));
    b->ClearCalls();
    EXPECT_TRUE(b->ActivateEGLSurface(kS1));
    EXPECT_TRUE(Trace().empty());
    EXPECT_EQ(b->ActivatingSurface(), kS2);
    EXPECT_FALSE(b->Record(kS1)->Activated);
    EXPECT_TRUE(b->CapsProbed());
    EXPECT_TRUE(WindowIs(b->WindowHandleNow(), Win(0x1004, 1, 2)));
}

// The activating surface is set before the hook and never reset afterwards:
// not by success, failure, release, reset or ReleaseEGLResources.
TEST_F(BackendObjectEglActivate, TheActivatingSurfaceIsSetBeforeTheHookAndNeverReset) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(b->CreateEGLWindowSurface(kS1, kWin1));
    ASSERT_EQ(Hooks().size(), 1u);
    EXPECT_EQ(Hooks()[0].State.Activating, kS1);
    EXPECT_EQ(b->ActivatingSurface(), kS1);
    ASSERT_TRUE(b->RegisterEGLWindowSurface(kS2, kWin2));
    b->failWindow = true;
    EXPECT_FALSE(b->ActivateEGLSurface(kS2));
    b->failWindow = false;
    ASSERT_EQ(Hooks().size(), 2u);
    EXPECT_EQ(Hooks()[1].State.Activating, kS2);
    EXPECT_EQ(b->ActivatingSurface(), kS2);
    // Releasing s2 runs only Forgotten: s1, not the activating s2, is the active surface.
    b->ClearCalls();
    b->ReleaseEGLSurface(kS2);
    EXPECT_EQ(Trace(), (Vector<String>{"Forgotten(s2)"}));
    EXPECT_EQ(b->ActiveSurface(), kS1);
    EXPECT_EQ(b->ActivatingSurface(), kS2);
    b->ResetEGLRuntimeState();
    EXPECT_EQ(b->ActivatingSurface(), kS2);
    b->ReleaseEGLResources();
    EXPECT_EQ(b->ActivatingSurface(), kS2);
}

// A window activation publishes the record's window, then calls InitWindowSurface once; the
// hook still sees the previous active surface and the record's old Activated.
TEST_F(BackendObjectEglActivate, AWindowActivationPublishesItsWindowBeforeTheHook) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(b->CreateEGLPbufferSurface(kS1, 8, 8));
    ASSERT_TRUE(b->RegisterEGLWindowSurface(kS2, kWin2));
    b->ClearCalls();
    ASSERT_TRUE(b->ActivateEGLSurface(kS2));
    EXPECT_EQ(Trace(), (Vector<String>{"InitWindow(s2)"}));
    const auto inside = Hooks()[0].State;
    EXPECT_EQ(inside.Activating, kS2);
    EXPECT_TRUE(WindowIs(inside.Window, kWin2));
    EXPECT_TRUE(inside.HasActive);
    EXPECT_EQ(inside.Active, kS1);
    EXPECT_EQ(inside.Kind, Kind::Pbuffer);
    ASSERT_TRUE(inside.Subject.has_value());
    EXPECT_FALSE(inside.Subject->Activated);
    EXPECT_EQ(b->ActiveSurface(), kS2);
    EXPECT_EQ(b->ActiveKind(), Kind::Window);
    EXPECT_TRUE(RecordIs(b->Record(kS2), WindowRecord(kWin2, 320, 200, true)));
    EXPECT_TRUE(WindowIs(b->WindowHandleNow(), kWin2));
}

// A pbuffer activation passes the record's extent and leaves the window handle alone, also
// when it is empty because the active window was released.
TEST_F(BackendObjectEglActivate, APbufferActivationPassesItsExtentAndKeepsTheWindowHandle) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(b->CreateEGLWindowSurface(kS1, kWin1));
    ASSERT_TRUE(b->RegisterEGLPbufferSurface(kS2, 30, 40));
    b->ClearCalls();
    ASSERT_TRUE(b->ActivateEGLSurface(kS2));
    EXPECT_EQ(Trace(), (Vector<String>{"InitPbuffer(s2,30,40)"}));
    const auto inside = Hooks()[0].State;
    EXPECT_EQ(inside.Activating, kS2);
    EXPECT_TRUE(WindowIs(inside.Window, kWin1));
    EXPECT_EQ(inside.Active, kS1);
    EXPECT_EQ(b->ActiveSurface(), kS2);
    EXPECT_EQ(b->ActiveKind(), Kind::Pbuffer);
    EXPECT_TRUE(WindowIs(b->WindowHandleNow(), kWin1));
    ASSERT_TRUE(b->CreateEGLWindowSurface(kS3, kWin3));
    b->ReleaseEGLSurface(kS3);
    ASSERT_TRUE(WindowIs(b->WindowHandleNow(), kNoWindow));
    b->ClearCalls();
    ASSERT_TRUE(b->ActivateEGLSurface(kS2));
    EXPECT_EQ(Trace(), (Vector<String>{"InitPbuffer(s2,30,40)"}));
    EXPECT_TRUE(WindowIs(b->WindowHandleNow(), kNoWindow));
}

// The base InitPbufferSurface refuses, so a backend without an override never
// activates a pbuffer (the registration stays).
TEST_F(BackendObjectEglActivate, TheBaseInitPbufferSurfaceRefuses) {
    ASSERT_TRUE(Init());
    b->pbufferInit = EglDouble::PbufferInit::Base;
    EXPECT_FALSE(b->CreateEGLPbufferSurface(kS1, 8, 8));
    EXPECT_EQ(Trace(), (Vector<String>{"InitPbuffer(s1,8,8)"}));
    EXPECT_FALSE(b->HasActiveSurface());
    EXPECT_TRUE(RecordIs(b->Record(kS1), PbufferRecord(8, 8)));
}

// A failed hook keeps the previous active surface, its kind, Activated and the probe flag;
// the activating surface and (window) the window handle stay overwritten.
TEST_F(BackendObjectEglActivate, AFailedHookKeepsThePreviousActiveSurface) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    ASSERT_TRUE(b->RegisterEGLWindowSurface(kS2, kWin2));
    ASSERT_TRUE(b->RegisterEGLPbufferSurface(kS3, 5, 6));
    b->ClearCalls();
    b->failWindow = true;
    EXPECT_FALSE(b->ActivateEGLSurface(kS2));
    EXPECT_EQ(b->ActiveSurface(), kS1);
    EXPECT_EQ(b->ActiveKind(), Kind::Window);
    EXPECT_TRUE(b->CapsProbed());
    EXPECT_FALSE(b->Record(kS2)->Activated);
    EXPECT_EQ(b->ActivatingSurface(), kS2);
    EXPECT_TRUE(WindowIs(b->WindowHandleNow(), kWin2));
    b->pbufferInit = EglDouble::PbufferInit::Fail;
    EXPECT_FALSE(b->ActivateEGLSurface(kS3));
    EXPECT_EQ(b->ActiveSurface(), kS1);
    EXPECT_TRUE(b->HasActiveSurface());
    EXPECT_EQ(b->ActiveKind(), Kind::Window);
    EXPECT_EQ(b->ActivatingSurface(), kS3);
    EXPECT_TRUE(WindowIs(b->WindowHandleNow(), kWin2));
    EXPECT_FALSE(b->Record(kS3)->Activated);
    EXPECT_TRUE(b->CapsProbed());
    EXPECT_EQ(Trace(), (Vector<String>{"InitWindow(s2)", "InitPbuffer(s3,5,6)"}));
}

// Success switches the active surface and its kind and leaves every thread's binding.
// The first activation of a record clears the probe flag; a later one does not.
TEST_F(BackendObjectEglActivate, ASuccessfulActivationSwitchesTheActiveSurfaceOnly) {
    Worker other;
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    ASSERT_TRUE(BindOn(other, kS1, kCtx2));
    ASSERT_TRUE(b->RegisterEGLPbufferSurface(kS2, 4, 4));
    b->ClearCalls();
    ASSERT_TRUE(b->ActivateEGLSurface(kS2));
    EXPECT_EQ(Trace(), (Vector<String>{"InitPbuffer(s2,4,4)"}));
    EXPECT_TRUE(b->HasActiveSurface());
    EXPECT_EQ(b->ActiveSurface(), kS2);
    EXPECT_EQ(b->ActiveKind(), Kind::Pbuffer);
    EXPECT_TRUE(b->Record(kS2)->Activated);
    EXPECT_FALSE(b->CapsProbed());
    EXPECT_TRUE(BoundTo(b->CurrentHere(), kDpy, kS1, kCtx));
    EXPECT_TRUE(BoundTo(CurrentOn(other), kDpy, kS1, kCtx2));
    EXPECT_EQ(b->CurrentCount(), 2u);
    ASSERT_TRUE(Bind(kS2));
    EXPECT_TRUE(b->CapsProbed());
    b->ClearCalls();
    ASSERT_TRUE(b->ActivateEGLSurface(kS1));
    EXPECT_EQ(Trace(), (Vector<String>{"InitWindow(s1)"}));
    EXPECT_TRUE(b->CapsProbed());
    EXPECT_EQ(b->ActiveKind(), Kind::Window);
    EXPECT_TRUE(ActiveInvariantHolds(*b));
}

// The activation reads the record again after the hook, so a hook that re-registers the
// surface it activates decides its kind. The record is assigned in place, not moved, so this is
// defined; no production hook does it.
TEST_F(BackendObjectEglActivate, AHookThatReRegistersItsSurfaceDecidesTheKind) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(b->RegisterEGLWindowSurface(kS1, kWin1));
    Bool reRegistered = false;
    b->onHook = [this, &reRegistered](const EglDouble::Call& call) {
        if (call.What != Hook::InitWindow || reRegistered) return;
        reRegistered = b->RegisterEGLPbufferSurface(call.Surface, 4, 4);
    };
    EXPECT_TRUE(b->ActivateEGLSurface(kS1));
    b->onHook = nullptr;
    EXPECT_TRUE(reRegistered);
    EXPECT_EQ(Trace(), (Vector<String>{"InitWindow(s1)"}));
    EXPECT_EQ(b->ActiveSurface(), kS1);
    EXPECT_EQ(b->ActiveKind(), Kind::Pbuffer);
    EXPECT_TRUE(RecordIs(b->Record(kS1), PbufferRecord(4, 4, true)));
    EXPECT_FALSE(b->CapsProbed());
}

// Activation uses the record's window as registered or resized, never the handle last
// given to SetWindowHandle.
TEST_F(BackendObjectEglActivate, ActivationUsesTheRecordsWindow) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(b->CreateEGLPbufferSurface(kS1, 8, 8));
    ASSERT_TRUE(b->RegisterEGLWindowSurface(kS2, kWin2));
    b->SetWindowHandle(kWin3);
    ASSERT_TRUE(b->ResizeEGLWindowSurface(kS2, 0, 77));
    EXPECT_TRUE(WindowIs(b->WindowHandleNow(), kWin3));
    b->ClearCalls();
    ASSERT_TRUE(b->ActivateEGLSurface(kS2));
    EXPECT_EQ(Trace(), (Vector<String>{"InitWindow(s2)"}));
    EXPECT_TRUE(WindowIs(Hooks()[0].State.Window, Win(0x1002, 0, 77)));
    EXPECT_TRUE(WindowIs(b->WindowHandleNow(), Win(0x1002, 0, 77)));
}

// ============================================ create ============================================

// A refused registration runs no hook (and does not activate an older record); a failed
// activation keeps the registration.
TEST_F(BackendObjectEglCreate, AFailedActivationKeepsTheRegistration) {
    EXPECT_FALSE(b->CreateEGLWindowSurface(kS1, kWin1));
    EXPECT_FALSE(b->CreateEGLPbufferSurface(kS2, 4, 4));
    EXPECT_TRUE(Trace().empty());
    EXPECT_TRUE(NoKnownRecord());
    ASSERT_TRUE(Init());
    b->failWindow = true;
    EXPECT_FALSE(b->CreateEGLWindowSurface(kS1, kWin1));
    b->failWindow = false;
    b->pbufferInit = EglDouble::PbufferInit::Fail;
    EXPECT_FALSE(b->CreateEGLPbufferSurface(kS2, 4, 4));
    b->pbufferInit = EglDouble::PbufferInit::Succeed;
    EXPECT_EQ(Trace(), (Vector<String>{"InitWindow(s1)", "InitPbuffer(s2,4,4)"}));
    EXPECT_TRUE(RecordIs(b->Record(kS1), WindowRecord(kWin1, 640, 480)));
    EXPECT_TRUE(RecordIs(b->Record(kS2), PbufferRecord(4, 4)));
    EXPECT_FALSE(b->HasActiveSurface());
    b->ClearCalls();
    EXPECT_FALSE(b->CreateEGLWindowSurface(kS1, Win(0, 8, 8)));
    EXPECT_FALSE(b->CreateEGLPbufferSurface(kS2, 0, 4));
    EXPECT_TRUE(Trace().empty());
    EXPECT_FALSE(b->HasActiveSurface());
}

// A second surface becomes active, the bindings stay, and the next bind probes once.
TEST_F(BackendObjectEglCreate, ASecondSurfaceBecomesActiveAndIsProbedOnce) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    b->ClearCalls();
    ASSERT_TRUE(b->CreateEGLWindowSurface(kS2, kWin2));
    EXPECT_EQ(Trace(), (Vector<String>{"InitWindow(s2)"}));
    EXPECT_EQ(b->ActiveSurface(), kS2);
    EXPECT_TRUE(BoundTo(b->CurrentHere(), kDpy, kS1, kCtx));
    EXPECT_FALSE(b->CapsProbed());
    b->ClearCalls();
    ASSERT_TRUE(Bind(kS2));
    EXPECT_EQ(Trace(), (Vector<String>{"InitCaps"}));
    b->ClearCalls();
    ASSERT_TRUE(Bind(kS2));
    EXPECT_TRUE(Trace().empty());
}

// Creating the active surface again replaces its record and short-circuits the
// activation; the replaced record's next real activation re-probes.
TEST_F(BackendObjectEglCreate, RecreatingTheActiveSurfaceReplacesItsRecordOnly) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    b->ClearCalls();
    EXPECT_TRUE(b->CreateEGLWindowSurface(kS1, kWin3));
    EXPECT_TRUE(Trace().empty());
    EXPECT_TRUE(RecordIs(b->Record(kS1), WindowRecord(kWin3, 64, 32)));
    EXPECT_TRUE(WindowIs(b->WindowHandleNow(), kWin1));
    EXPECT_TRUE(b->CapsProbed());
    ASSERT_TRUE(BoundWindow(kS2, kWin2));
    b->ClearCalls();
    ASSERT_TRUE(Bind(kS1));
    EXPECT_EQ(Trace(), (Vector<String>{"InitWindow(s1)", "InitCaps"}));
    EXPECT_TRUE(WindowIs(b->WindowHandleNow(), kWin3));
    b->ClearCalls();
    EXPECT_TRUE(b->CreateEGLPbufferSurface(kS1, 9, 9));
    EXPECT_TRUE(Trace().empty());
    EXPECT_TRUE(RecordIs(b->Record(kS1), PbufferRecord(9, 9)));
    EXPECT_EQ(b->ActiveKind(), Kind::Window);
}

// A create runs exactly the init hook of its kind, never the probe.
TEST_F(BackendObjectEglCreate, ACreateRunsExactlyTheInitHookOfItsKind) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(b->CreateEGLWindowSurface(kS1, kWin1));
    EXPECT_EQ(Trace(), (Vector<String>{"InitWindow(s1)"}));
    b->ClearCalls();
    ASSERT_TRUE(b->CreateEGLPbufferSurface(kS2, 30, 40));
    EXPECT_EQ(Trace(), (Vector<String>{"InitPbuffer(s2,30,40)"}));
    EXPECT_TRUE(RecordIs(b->Record(kS2), PbufferRecord(30, 40, true)));
}

// ============================================ resize ============================================

// An unknown surface is refused, with or without a display.
TEST_F(BackendObjectEglResize, AnUnknownSurfaceIsRefused) {
    EXPECT_FALSE(b->ResizeEGLWindowSurface(kS9, 1, 1));
    ASSERT_TRUE(Init());
    EXPECT_FALSE(b->ResizeEGLWindowSurface(kS9, 1, 1));
    EXPECT_FALSE(b->Record(kS9).has_value());
    EXPECT_TRUE(Trace().empty());
}

// The record's window takes the raw extent and the record's own extent is clamped to at
// least 1; nothing else of the record changes, pbuffers included, and no hook runs.
TEST_F(BackendObjectEglResize, AResizeWritesTheRawAndTheClampedExtent) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    b->ReleaseEGLSurface(kS1);
    ASSERT_TRUE(b->RegisterEGLPbufferSurface(kS2, 30, 40));
    b->ClearCalls();
    ASSERT_TRUE(b->ResizeEGLWindowSurface(kS1, 0, 7));
    EXPECT_TRUE(RecordIs(b->Record(kS1), WindowRecord(Win(0x1001, 0, 7), 1, 7, true, true)));
    ASSERT_TRUE(b->ResizeEGLWindowSurface(kS2, 0, 0));
    EXPECT_TRUE(RecordIs(b->Record(kS2), PbufferRecord(1, 1, false, false, Win(0, 0, 0, WindowBackend::Unknown))));
    ASSERT_TRUE(b->ResizeEGLWindowSurface(kS2, 100, 0));
    EXPECT_TRUE(RecordIs(b->Record(kS2), PbufferRecord(100, 1, false, false, Win(0, 100, 0, WindowBackend::Unknown))));
    ASSERT_TRUE(b->ResizeEGLWindowSurface(kS2, 0, 50));
    EXPECT_TRUE(RecordIs(b->Record(kS2), PbufferRecord(1, 50, false, false, Win(0, 0, 50, WindowBackend::Unknown))));
    EXPECT_TRUE(Trace().empty());
    EXPECT_TRUE(b->CapsProbed());
    EXPECT_EQ(b->ActiveSurface(), kS1);
}

// A quirk: a resize above INT_MAX wraps the record's extent to a negative EGLint too.
TEST_F(BackendObjectEglResize, AnExtentAboveIntMaxWrapsNegative) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(b->RegisterEGLPbufferSurface(kS2, 4, 4));
    ASSERT_TRUE(b->ResizeEGLWindowSurface(kS2, 0xFFFFFFFFu, 0x80000000u));
    const WindowHandle raw = Win(0, 0xFFFFFFFFu, 0x80000000u, WindowBackend::Unknown);
    EXPECT_TRUE(RecordIs(b->Record(kS2), PbufferRecord(-1, kIntMin, false, false, raw)));
}

// Only the active surface's resize edits the window handle, and only its extent - for an
// active pbuffer, whatever handle happens to be there.
TEST_F(BackendObjectEglResize, OnlyTheActiveSurfacesResizeEditsTheWindowHandle) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(b->CreateEGLWindowSurface(kS1, kWin1));
    ASSERT_TRUE(b->RegisterEGLWindowSurface(kS2, kWin2));
    ASSERT_TRUE(b->ResizeEGLWindowSurface(kS2, 11, 12));
    EXPECT_TRUE(WindowIs(b->WindowHandleNow(), kWin1));
    ASSERT_TRUE(b->ResizeEGLWindowSurface(kS1, 13, 14));
    EXPECT_TRUE(WindowIs(b->WindowHandleNow(), Win(0x1001, 13, 14)));
    ASSERT_TRUE(b->CreateEGLPbufferSurface(kS3, 4, 4));
    EXPECT_TRUE(WindowIs(b->WindowHandleNow(), Win(0x1001, 13, 14)));
    ASSERT_TRUE(b->ResizeEGLWindowSurface(kS3, 21, 22));
    EXPECT_TRUE(WindowIs(b->WindowHandleNow(), Win(0x1001, 21, 22)));
    EXPECT_TRUE(RecordIs(b->Record(kS3), PbufferRecord(21, 22, true, false, Win(0, 21, 22, WindowBackend::Unknown))));
    b->SetWindowHandle(Win(0x1004, 1, 1, WindowBackend::X11));
    ASSERT_TRUE(b->ResizeEGLWindowSurface(kS3, 5, 0));
    EXPECT_TRUE(WindowIs(b->WindowHandleNow(), Win(0x1004, 5, 0, WindowBackend::X11)));
}

// It is the ACTIVE surface whose resize edits the window handle, not the activating one.
// After a failed activation of s2 the handle holds s2's window: s2's resize leaves it, and the
// active s1's resize edits its extent.
TEST_F(BackendObjectEglResize, TheActiveSurfaceNotTheActivatingOneEditsTheWindowHandle) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(b->CreateEGLWindowSurface(kS1, kWin1));
    ASSERT_TRUE(b->RegisterEGLWindowSurface(kS2, kWin2));
    b->failWindow = true;
    ASSERT_FALSE(b->ActivateEGLSurface(kS2));
    b->failWindow = false;
    ASSERT_EQ(b->ActivatingSurface(), kS2);
    ASSERT_TRUE(b->ResizeEGLWindowSurface(kS2, 7, 9));
    EXPECT_TRUE(WindowIs(b->WindowHandleNow(), kWin2));
    EXPECT_TRUE(RecordIs(b->Record(kS2), WindowRecord(Win(0x1002, 7, 9), 7, 9)));
    ASSERT_TRUE(b->ResizeEGLWindowSurface(kS1, 5, 6));
    EXPECT_TRUE(WindowIs(b->WindowHandleNow(), Win(0x1002, 5, 6)));
    EXPECT_TRUE(RecordIs(b->Record(kS1), WindowRecord(Win(0x1001, 5, 6), 5, 6, true)));
}

// A resize applies at the next activation and never re-runs the init hook itself.
TEST_F(BackendObjectEglResize, AResizeTakesEffectAtTheNextActivation) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(b->CreateEGLWindowSurface(kS1, kWin1));
    ASSERT_TRUE(b->RegisterEGLPbufferSurface(kS2, 30, 40));
    b->ClearCalls();
    ASSERT_TRUE(b->ResizeEGLWindowSurface(kS2, 50, 0));
    EXPECT_TRUE(Trace().empty());
    ASSERT_TRUE(b->ActivateEGLSurface(kS2));
    EXPECT_EQ(Trace(), (Vector<String>{"InitPbuffer(s2,50,1)"}));
    ASSERT_TRUE(b->ResizeEGLWindowSurface(kS2, 60, 70));
    EXPECT_EQ(Trace(), (Vector<String>{"InitPbuffer(s2,50,1)"}));
    EXPECT_EQ(b->ActiveSurface(), kS2);
}

// ========================================= make current =========================================

// A release request succeeds with any display, before any display and after
// ReleaseEGLResources, and releases this thread's binding only.
TEST_F(BackendObjectEglMakeCurrent, AReleaseRequestSucceedsWithAnyDisplay) {
    Worker other;
    EXPECT_TRUE(Release());
    EXPECT_TRUE(b->MakeEGLCurrent(kDpy2, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    ASSERT_TRUE(BindOn(other, kS1, kCtx2));
    b->ClearCalls();
    EXPECT_TRUE(b->MakeEGLCurrent(kDpy2, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
    EXPECT_FALSE(b->CurrentHere().has_value());
    EXPECT_TRUE(BoundTo(CurrentOn(other), kDpy, kS1, kCtx2));
    EXPECT_EQ(b->ActiveSurface(), kS1);
    EXPECT_TRUE(Trace().empty());
    ASSERT_TRUE(Bind(kS1));
    b->ReleaseEGLResources();
    EXPECT_TRUE(Release());
    EXPECT_TRUE(Trace().empty());
}

// A bind names the initialized display; a refused one keeps the previous binding.
// Before any display there is no record either, so the first refusal has more than one cause;
// where the display check sits is pinned by the next case.
TEST_F(BackendObjectEglMakeCurrent, ABindNeedsTheInitializedDisplay) {
    EXPECT_FALSE(b->MakeEGLCurrent(kDpy, kS1, kS1, kCtx));
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    b->ClearCalls();
    EXPECT_FALSE(b->MakeEGLCurrent(kDpy2, kS1, kS1, kCtx2));
    EXPECT_FALSE(b->MakeEGLCurrent(EGL_NO_DISPLAY, kS1, kS1, kCtx2));
    EXPECT_TRUE(BoundTo(b->CurrentHere(), kDpy, kS1, kCtx));
    EXPECT_TRUE(Trace().empty());
}

// The display is checked before any activation, lazy or not: a mismatched bind
// onto a registered surface that is not active runs no hook and changes nothing.
TEST_F(BackendObjectEglMakeCurrent, ADisplayMismatchIsRefusedBeforeAnyActivation) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(b->RegisterEGLWindowSurface(kS1, kWin1));
    ASSERT_TRUE(b->RegisterEGLWindowSurface(kS2, kWin2));
    EXPECT_FALSE(b->MakeEGLCurrent(kDpy2, kS1, kS1, kCtx));
    EXPECT_TRUE(Trace().empty());
    EXPECT_FALSE(b->HasActiveSurface());
    EXPECT_EQ(b->ActivatingSurface(), EGL_NO_SURFACE);
    EXPECT_TRUE(WindowIs(b->WindowHandleNow(), kNoWindow));
    EXPECT_FALSE(b->Record(kS1)->Activated);
    ASSERT_TRUE(Bind(kS1));
    b->ClearCalls();
    EXPECT_FALSE(b->MakeEGLCurrent(kDpy2, kS2, kS2, kCtx));
    EXPECT_TRUE(Trace().empty());
    EXPECT_EQ(b->ActiveSurface(), kS1);
    EXPECT_EQ(b->ActivatingSurface(), kS1);
    EXPECT_TRUE(WindowIs(b->WindowHandleNow(), kWin1));
    EXPECT_FALSE(b->Record(kS2)->Activated);
    EXPECT_TRUE(b->CapsProbed());
    EXPECT_TRUE(BoundTo(b->CurrentHere(), kDpy, kS1, kCtx));
}

// With no active surface a bind activates draw lazily, but only when draw == read.
TEST_F(BackendObjectEglMakeCurrent, WithNoActiveSurfaceABindActivatesDrawLazily) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(b->RegisterEGLWindowSurface(kS1, kWin1));
    ASSERT_TRUE(b->RegisterEGLPbufferSurface(kS2, 4, 4));
    EXPECT_FALSE(b->MakeEGLCurrent(kDpy, kS1, kS2, kCtx));
    EXPECT_FALSE(b->MakeEGLCurrent(kDpy, kS9, kS1, kCtx));
    EXPECT_TRUE(Trace().empty());
    EXPECT_FALSE(b->HasActiveSurface());
    EXPECT_EQ(b->ActivatingSurface(), EGL_NO_SURFACE);
    EXPECT_TRUE(Bind(kS1));
    EXPECT_EQ(Trace(), (Vector<String>{"InitWindow(s1)", "InitCaps"}));
    EXPECT_EQ(b->ActiveSurface(), kS1);
    EXPECT_TRUE(BoundTo(b->CurrentHere(), kDpy, kS1, kCtx));
}

TEST_F(BackendObjectEglMakeCurrent, ALazyPbufferBindRunsItsInitHookThenTheProbe) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(b->RegisterEGLPbufferSurface(kS2, 4, 5));
    EXPECT_TRUE(Bind(kS2));
    EXPECT_EQ(Trace(), (Vector<String>{"InitPbuffer(s2,4,5)", "InitCaps"}));
    EXPECT_EQ(b->ActiveKind(), Kind::Pbuffer);
}

// A failed lazy activation refuses the bind and keeps its side effects; an
// unregistered draw == read is refused with no write.
TEST_F(BackendObjectEglMakeCurrent, AFailedLazyActivationRefusesTheBind) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(b->RegisterEGLWindowSurface(kS1, kWin1));
    b->failWindow = true;
    EXPECT_FALSE(Bind(kS1));
    EXPECT_EQ(Trace(), (Vector<String>{"InitWindow(s1)"}));
    EXPECT_EQ(b->ActivatingSurface(), kS1);
    EXPECT_TRUE(WindowIs(b->WindowHandleNow(), kWin1));
    EXPECT_FALSE(b->CurrentHere().has_value());
    EXPECT_FALSE(b->HasActiveSurface());
    EXPECT_FALSE(b->Record(kS1)->Activated);
    EXPECT_FALSE(Bind(kS9));
    EXPECT_EQ(b->ActivatingSurface(), kS1);
    EXPECT_EQ(Trace(), (Vector<String>{"InitWindow(s1)"}));
}

// An unregistered draw or read, a surfaceless bind and a partial release are refused,
// with a surface active and with none.
TEST_F(BackendObjectEglMakeCurrent, UnregisteredSurfacelessAndPartialBindsAreRefused) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    b->ClearCalls();
    EXPECT_FALSE(b->MakeEGLCurrent(kDpy, kS1, kS9, kCtx));
    EXPECT_FALSE(b->MakeEGLCurrent(kDpy, kS9, kS1, kCtx));
    EXPECT_FALSE(b->MakeEGLCurrent(kDpy, kS9, kS9, kCtx));
    EXPECT_FALSE(b->MakeEGLCurrent(kDpy, EGL_NO_SURFACE, EGL_NO_SURFACE, kCtx));
    EXPECT_FALSE(b->MakeEGLCurrent(kDpy, kS1, EGL_NO_SURFACE, EGL_NO_CONTEXT));
    EXPECT_FALSE(b->MakeEGLCurrent(kDpy, EGL_NO_SURFACE, kS1, EGL_NO_CONTEXT));
    EXPECT_TRUE(BoundTo(b->CurrentHere(), kDpy, kS1, kCtx));
    EXPECT_EQ(b->ActiveSurface(), kS1);
    EXPECT_TRUE(Trace().empty());
    // Nothing active: the active pbuffer is released, s1 stays bound here.
    ASSERT_TRUE(b->CreateEGLPbufferSurface(kS2, 4, 4));
    b->ReleaseEGLSurface(kS2);
    ASSERT_FALSE(b->HasActiveSurface());
    b->ClearCalls();
    EXPECT_FALSE(b->MakeEGLCurrent(kDpy, EGL_NO_SURFACE, EGL_NO_SURFACE, kCtx));
    EXPECT_TRUE(BoundTo(b->CurrentHere(), kDpy, kS1, kCtx));
    EXPECT_TRUE(Trace().empty());
}

// Separate draw and read surfaces are refused with no hook.
TEST_F(BackendObjectEglMakeCurrent, SeparateDrawAndReadSurfacesAreRefused) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    ASSERT_TRUE(b->RegisterEGLPbufferSurface(kS2, 4, 4));
    b->ClearCalls();
    EXPECT_FALSE(b->MakeEGLCurrent(kDpy, kS2, kS1, kCtx));
    EXPECT_FALSE(b->MakeEGLCurrent(kDpy, kS1, kS2, kCtx));
    EXPECT_TRUE(Trace().empty());
    EXPECT_EQ(b->ActiveSurface(), kS1);
    EXPECT_TRUE(BoundTo(b->CurrentHere(), kDpy, kS1, kCtx));
}

// A bind onto another registered surface activates it; a failed activation refuses.
TEST_F(BackendObjectEglMakeCurrent, ABindOntoAnotherSurfaceActivatesIt) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    ASSERT_TRUE(b->RegisterEGLPbufferSurface(kS2, 6, 7));
    b->ClearCalls();
    b->pbufferInit = EglDouble::PbufferInit::Fail;
    EXPECT_FALSE(Bind(kS2));
    EXPECT_EQ(Trace(), (Vector<String>{"InitPbuffer(s2,6,7)"}));
    EXPECT_EQ(b->ActiveSurface(), kS1);
    EXPECT_TRUE(BoundTo(b->CurrentHere(), kDpy, kS1, kCtx));
    b->pbufferInit = EglDouble::PbufferInit::Succeed;
    b->ClearCalls();
    EXPECT_TRUE(Bind(kS2));
    EXPECT_EQ(Trace(), (Vector<String>{"InitPbuffer(s2,6,7)", "InitCaps"}));
    EXPECT_EQ(b->ActiveSurface(), kS2);
    EXPECT_TRUE(BoundTo(b->CurrentHere(), kDpy, kS2, kCtx));
}

// A quirk: valid surfaces with EGL_NO_CONTEXT are refused only AFTER draw was activated,
// lazily or by the switch, which clears the probe flag; the previous binding stays.
TEST_F(BackendObjectEglMakeCurrent, NoContextIsRefusedAfterTheActivation) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(b->RegisterEGLWindowSurface(kS1, kWin1));
    EXPECT_FALSE(b->MakeEGLCurrent(kDpy, kS1, kS1, EGL_NO_CONTEXT));
    EXPECT_EQ(Trace(), (Vector<String>{"InitWindow(s1)"}));
    EXPECT_EQ(b->ActiveSurface(), kS1);
    EXPECT_TRUE(b->Record(kS1)->Activated);
    EXPECT_FALSE(b->CapsProbed());
    EXPECT_FALSE(b->CurrentHere().has_value());
    ASSERT_TRUE(Bind(kS1));
    ASSERT_TRUE(b->RegisterEGLWindowSurface(kS2, kWin2));
    b->ClearCalls();
    EXPECT_FALSE(b->MakeEGLCurrent(kDpy, kS2, kS2, EGL_NO_CONTEXT));
    EXPECT_EQ(Trace(), (Vector<String>{"InitWindow(s2)"}));
    EXPECT_EQ(b->ActiveSurface(), kS2);
    EXPECT_TRUE(b->Record(kS2)->Activated);
    EXPECT_FALSE(b->CapsProbed());
    EXPECT_TRUE(BoundTo(b->CurrentHere(), kDpy, kS1, kCtx));
}

// A failed probe refuses the bind, keeps the previous binding (and its pending destroy)
// and is retried by every bind until it succeeds.
TEST_F(BackendObjectEglMakeCurrent, AFailedProbeRefusesTheBindAndIsRetried) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    ASSERT_TRUE(b->CreateEGLWindowSurface(kS2, kWin2));
    b->ReleaseEGLSurface(kS1);
    b->failCaps = true;
    b->ClearCalls();
    EXPECT_FALSE(Bind(kS2));
    EXPECT_EQ(Trace(), (Vector<String>{"InitCaps"}));
    EXPECT_FALSE(b->CapsProbed());
    EXPECT_TRUE(BoundTo(b->CurrentHere(), kDpy, kS1, kCtx));
    EXPECT_TRUE(RecordIs(b->Record(kS1), WindowRecord(kWin1, 640, 480, true, true)));
    EXPECT_FALSE(Bind(kS2));
    EXPECT_EQ(Trace(), (Vector<String>{"InitCaps", "InitCaps"}));
    b->failCaps = false;
    b->ClearCalls();
    EXPECT_TRUE(Bind(kS2));
    EXPECT_EQ(Trace(), (Vector<String>{"InitCaps", "Forgotten(s1)"}));
    EXPECT_TRUE(b->CapsProbed());
    EXPECT_TRUE(BoundTo(b->CurrentHere(), kDpy, kS2, kCtx));
    EXPECT_FALSE(b->Record(kS1).has_value());
    b->ClearCalls();
    EXPECT_TRUE(Bind(kS2));
    EXPECT_TRUE(Trace().empty());
}

// The activation that came before a failed probe stays.
TEST_F(BackendObjectEglMakeCurrent, AFailedProbeKeepsTheActivationBeforeIt) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(b->RegisterEGLWindowSurface(kS1, kWin1));
    b->failCaps = true;
    EXPECT_FALSE(Bind(kS1));
    EXPECT_EQ(Trace(), (Vector<String>{"InitWindow(s1)", "InitCaps"}));
    EXPECT_EQ(b->ActiveSurface(), kS1);
    EXPECT_TRUE(b->Record(kS1)->Activated);
    EXPECT_FALSE(b->CapsProbed());
    EXPECT_FALSE(b->CurrentHere().has_value());
}

// A successful bind first releases this thread's previous binding, which can
// complete the pending destroy of the surface it named, and then records the new binding.
TEST_F(BackendObjectEglMakeCurrent, ABindReleasesThePreviousBindingFirst) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    ASSERT_TRUE(b->CreateEGLPbufferSurface(kS2, 4, 4));
    b->ReleaseEGLSurface(kS1);
    b->ClearCalls();
    EXPECT_TRUE(b->MakeEGLCurrent(kDpy, kS2, kS2, kCtx2));
    EXPECT_EQ(Trace(), (Vector<String>{"InitCaps", "Forgotten(s1)"}));
    const auto forgotten = Hooks()[1].State;
    EXPECT_FALSE(forgotten.Current.has_value());
    EXPECT_FALSE(forgotten.Subject.has_value());
    EXPECT_TRUE(BoundTo(b->CurrentHere(), kDpy, kS2, kCtx2));
    EXPECT_FALSE(b->Record(kS1).has_value());
    EXPECT_EQ(b->ActiveSurface(), kS2);
}

// An identical rebind runs no hook; one thread has one binding.
TEST_F(BackendObjectEglMakeCurrent, AnIdenticalRebindRunsNoHook) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    b->ClearCalls();
    EXPECT_TRUE(Bind(kS1));
    EXPECT_TRUE(Trace().empty());
    EXPECT_TRUE(BoundTo(b->CurrentHere(), kDpy, kS1, kCtx));
    EXPECT_TRUE(Bind(kS1, kCtx2));
    EXPECT_TRUE(BoundTo(b->CurrentHere(), kDpy, kS1, kCtx2));
    EXPECT_EQ(b->CurrentCount(), 1u);
    EXPECT_TRUE(Trace().empty());
}

TEST_F(BackendObjectEglMakeCurrent, TheFirstBindAfterACreateOnlyProbes) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(b->CreateEGLWindowSurface(kS1, kWin1));
    b->ClearCalls();
    EXPECT_TRUE(Bind(kS1));
    EXPECT_EQ(Trace(), (Vector<String>{"InitCaps"}));
}

// No refused bind releases the previous binding, so its pending destroy waits.
// Each refusal is checked on its own: only the no-context and the failed-probe ones get as far
// as a hook.
TEST_F(BackendObjectEglMakeCurrent, ARefusedBindKeepsThePreviousBinding) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    ASSERT_TRUE(b->RegisterEGLWindowSurface(kS2, kWin2));
    b->ReleaseEGLSurface(kS1);
    b->ClearCalls();
    EXPECT_FALSE(b->MakeEGLCurrent(kDpy2, kS2, kS2, kCtx));
    EXPECT_TRUE(Trace().empty());
    EXPECT_FALSE(b->MakeEGLCurrent(kDpy, kS2, kS1, kCtx));
    EXPECT_TRUE(Trace().empty());
    EXPECT_FALSE(b->MakeEGLCurrent(kDpy, kS2, kS9, kCtx));
    EXPECT_TRUE(Trace().empty());
    EXPECT_EQ(b->ActiveSurface(), kS1);
    EXPECT_FALSE(b->MakeEGLCurrent(kDpy, kS2, kS2, EGL_NO_CONTEXT));
    EXPECT_EQ(Trace(), (Vector<String>{"InitWindow(s2)"}));
    b->failCaps = true;
    b->ClearCalls();
    EXPECT_FALSE(Bind(kS2));
    EXPECT_EQ(Trace(), (Vector<String>{"InitCaps"}));
    EXPECT_TRUE(RecordIs(b->Record(kS1), WindowRecord(kWin1, 640, 480, true, true)));
    EXPECT_TRUE(BoundTo(b->CurrentHere(), kDpy, kS1, kCtx));
}

// A quirk, pinned as it is: rebinding a pending surface current only on this thread
// succeeds, but the bind's own release destroys it: no active surface, an entry naming a dropped
// surface, and the next swap of it fails.
TEST_F(BackendObjectEglMakeCurrent, RebindingAPendingSurfaceDestroysItButSucceeds) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    b->ReleaseEGLSurface(kS1);
    b->ClearCalls();
    EXPECT_TRUE(Bind(kS1));
    EXPECT_EQ(Trace(), (Vector<String>{"Released(s1)", "Forgotten(s1)"}));
    EXPECT_FALSE(b->HasActiveSurface());
    EXPECT_FALSE(b->Record(kS1).has_value());
    EXPECT_TRUE(BoundTo(b->CurrentHere(), kDpy, kS1, kCtx));
    b->ClearCalls();
    EXPECT_FALSE(Swap(kS1));
    EXPECT_TRUE(TraceWithTableReads().empty());
    EXPECT_EQ(b->PresentCount(), 0u);
}

// A quirk: the same when the pending surface is not active - it is activated first.
TEST_F(BackendObjectEglMakeCurrent, RebindingAPendingInactiveSurfaceActivatesThenDestroysIt) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    ASSERT_TRUE(b->CreateEGLWindowSurface(kS2, kWin2));
    b->ReleaseEGLSurface(kS1);
    b->ClearCalls();
    EXPECT_TRUE(Bind(kS1));
    EXPECT_EQ(Trace(), (Vector<String>{"InitWindow(s1)", "InitCaps", "Released(s1)", "Forgotten(s1)"}));
    EXPECT_FALSE(b->HasActiveSurface());
    EXPECT_TRUE(ActiveInvariantHolds(*b));
    EXPECT_TRUE(BoundTo(b->CurrentHere(), kDpy, kS1, kCtx));
}

// ======================================= capability probe =======================================

// Several surfaces first activated before the first bind cost one probe; switching back to
// an activated surface costs none.
TEST_F(BackendObjectEglCaps, SurfacesCreatedBeforeTheFirstBindAreProbedOnce) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(b->CreateEGLWindowSurface(kS1, kWin1));
    ASSERT_TRUE(b->CreateEGLPbufferSurface(kS2, 4, 4));
    ASSERT_TRUE(b->CreateEGLWindowSurface(kS3, kWin3));
    b->ClearCalls();
    EXPECT_TRUE(Bind(kS3));
    EXPECT_EQ(Trace(), (Vector<String>{"InitCaps"}));
    EXPECT_TRUE(Bind(kS1));
    EXPECT_TRUE(Bind(kS2));
    EXPECT_EQ(Trace(), (Vector<String>{"InitCaps", "InitWindow(s1)", "InitPbuffer(s2,4,4)"}));
}

// Only a record's first activation, a reset and ReleaseEGLResources clear the
// flag. A resize, a window handle, the same display again, re-activating an activated record and
// releasing the active surface leave it, so a lazy bind afterwards does not probe.
TEST_F(BackendObjectEglCaps, OnlyFirstActivationsAndResetsClearTheProbeFlag) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    ASSERT_TRUE(b->ResizeEGLWindowSurface(kS1, 3, 3));
    EXPECT_TRUE(b->CapsProbed());
    b->SetWindowHandle(kWin2);
    EXPECT_TRUE(b->CapsProbed());
    ASSERT_TRUE(b->InitializeEGLDisplay(kDpy, nullptr, nullptr));
    EXPECT_TRUE(b->CapsProbed());
    ASSERT_TRUE(b->RegisterEGLWindowSurface(kS2, kWin2));
    ASSERT_TRUE(Bind(kS2));
    ASSERT_TRUE(b->CapsProbed());
    ASSERT_TRUE(b->ActivateEGLSurface(kS1));
    EXPECT_TRUE(b->CapsProbed());
    ASSERT_TRUE(Release());
    b->ReleaseEGLSurface(kS1);
    EXPECT_FALSE(b->HasActiveSurface());
    EXPECT_TRUE(b->CapsProbed());
    b->ClearCalls();
    ASSERT_TRUE(Bind(kS2));
    EXPECT_EQ(Trace(), (Vector<String>{"InitWindow(s2)"}));
    b->ResetEGLRuntimeState();
    EXPECT_FALSE(b->CapsProbed());
    b->ClearCalls();
    ASSERT_TRUE(Bind(kS2));
    EXPECT_EQ(Trace(), (Vector<String>{"InitWindow(s2)", "InitCaps"}));
    ASSERT_TRUE(b->CapsProbed());
    b->ReleaseEGLResources();
    EXPECT_FALSE(b->CapsProbed());
}

// The flag is the base's own bookkeeping in MakeEGLCurrent, not part of InitCapabilities: a
// direct call (as the server's control op makes) leaves it, so the next bind still probes. This
// guards the split; only InitCapabilities ceasing to be a plain public virtual breaks it.
TEST_F(BackendObjectEglCaps, ADirectProbeLeavesTheFlagAlone) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(b->CreateEGLWindowSurface(kS1, kWin1));
    EXPECT_TRUE(b->InitCapabilities());
    EXPECT_FALSE(b->CapsProbed());
    b->ClearCalls();
    ASSERT_TRUE(Bind(kS1));
    EXPECT_EQ(Trace(), (Vector<String>{"InitCaps"}));
    b->failCaps = true;
    EXPECT_FALSE(b->InitCapabilities());
    EXPECT_TRUE(b->CapsProbed());
}

// A swap that activates a re-registered surface clears the flag but does not probe; the next
// bind does.
TEST_F(BackendObjectEglCaps, ASwapActivationNeverProbes) {
    Worker other;
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    ASSERT_TRUE(b->CreateEGLPbufferSurface(kS2, 4, 4));
    ASSERT_TRUE(BindOn(other, kS2));
    ASSERT_TRUE(b->RegisterEGLWindowSurface(kS1, kWin1));
    ASSERT_TRUE(b->CapsProbed());
    b->ClearCalls();
    EXPECT_TRUE(Swap(kS1));
    EXPECT_EQ(Trace(), (Vector<String>{"InitWindow(s1)", "Present"}));
    EXPECT_FALSE(b->CapsProbed());
    b->ClearCalls();
    EXPECT_TRUE(Bind(kS1));
    EXPECT_EQ(Trace(), (Vector<String>{"InitCaps"}));
}

// ============================================= swap =============================================

// A swap names the initialized display, and the binding's own display. Before any
// display there is no binding either, so the first refusal has more than one cause; where the
// display check sits is pinned by the next case.
TEST_F(BackendObjectEglSwap, ASwapNeedsTheInitializedDisplay) {
    EXPECT_FALSE(Swap(kS1));
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    b->ClearCalls();
    EXPECT_FALSE(b->SwapEGLBuffers(kDpy2, kS1));
    ASSERT_TRUE(b->InitializeEGLDisplay(kDpy2, nullptr, nullptr));
    EXPECT_FALSE(b->SwapEGLBuffers(kDpy, kS1));  // not the display any more
    EXPECT_FALSE(b->SwapEGLBuffers(kDpy2, kS1)); // the binding names kDpy
    EXPECT_TRUE(TraceWithTableReads().empty());
    EXPECT_EQ(b->PresentCount(), 0u);
}

// The display is checked before the re-activation: a swap of this thread's surface on
// a display that was replaced runs no hook, although that surface is not the active one.
TEST_F(BackendObjectEglSwap, ADisplayMismatchIsRefusedBeforeTheReactivation) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    ASSERT_TRUE(b->CreateEGLPbufferSurface(kS2, 4, 4));
    ASSERT_TRUE(b->InitializeEGLDisplay(kDpy2, nullptr, nullptr));
    b->ClearCalls();
    EXPECT_FALSE(Swap(kS1));
    EXPECT_TRUE(TraceWithTableReads().empty());
    EXPECT_EQ(b->ActiveSurface(), kS2);
    EXPECT_EQ(b->ActivatingSurface(), kS2);
    EXPECT_EQ(b->PresentCount(), 0u);
}

// A swap of a surface other than this thread's draw surface is refused before any
// activation.
TEST_F(BackendObjectEglSwap, ASwapOfASurfaceNotBoundHereIsRefused) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    ASSERT_TRUE(b->CreateEGLWindowSurface(kS2, kWin2));
    ASSERT_TRUE(b->RegisterEGLPbufferSurface(kS3, 2, 2));
    b->ClearCalls();
    EXPECT_FALSE(Swap(kS2));
    EXPECT_FALSE(Swap(kS3));
    EXPECT_TRUE(TraceWithTableReads().empty());
    EXPECT_EQ(b->ActiveSurface(), kS2);
    EXPECT_EQ(b->PresentCount(), 0u);
}

// A swap re-activates this thread's surface when another one or none is active
// (no probe), reads the function table after that and presents once on this thread. A failed
// re-activation refuses, and the retry activates again although s1 is the activating surface.
TEST_F(BackendObjectEglSwap, ASwapReactivatesThisThreadsSurface) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    ASSERT_TRUE(b->CreateEGLPbufferSurface(kS2, 4, 4));
    b->ClearCalls();
    EXPECT_TRUE(Swap(kS1));
    EXPECT_EQ(TraceWithTableReads(), (Vector<String>{"InitWindow(s1)", "Functions", "Present"}));
    EXPECT_EQ(b->ActiveSurface(), kS1);
    EXPECT_TRUE(WindowIs(b->WindowHandleNow(), kWin1));
    EXPECT_TRUE(AllCallsOn(Self()));
    EXPECT_EQ(Hooks()[1].State.Active, kS1);
    b->ClearCalls();
    EXPECT_TRUE(Swap(kS1));
    EXPECT_EQ(Trace(), (Vector<String>{"Present"}));
    // Nothing active: the active pbuffer is released.
    ASSERT_TRUE(b->CreateEGLPbufferSurface(kS3, 2, 2));
    b->ReleaseEGLSurface(kS3);
    ASSERT_FALSE(b->HasActiveSurface());
    b->ClearCalls();
    EXPECT_TRUE(Swap(kS1));
    EXPECT_EQ(Trace(), (Vector<String>{"InitWindow(s1)", "Present"}));
    ASSERT_TRUE(b->ActivateEGLSurface(kS2));
    b->failWindow = true;
    b->ClearCalls();
    EXPECT_FALSE(Swap(kS1));
    EXPECT_EQ(Trace(), (Vector<String>{"InitWindow(s1)"}));
    EXPECT_EQ(b->ActiveSurface(), kS2);
    EXPECT_EQ(b->ActivatingSurface(), kS1);
    b->failWindow = false;
    b->ClearCalls();
    EXPECT_TRUE(Swap(kS1));
    EXPECT_EQ(Trace(), (Vector<String>{"InitWindow(s1)", "Present"}));
    EXPECT_EQ(b->ActiveSurface(), kS1);
    EXPECT_EQ(b->PresentCount(), 4u);
}

// The function table is read once, after the activation; a null Present refuses and
// the activation stays.
TEST_F(BackendObjectEglSwap, ANullPresentRefusesAfterTheActivation) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    ASSERT_TRUE(b->CreateEGLPbufferSurface(kS2, 4, 4));
    b->SetPresentNull(true);
    b->ClearCalls();
    EXPECT_FALSE(Swap(kS1));
    EXPECT_EQ(TraceWithTableReads(), (Vector<String>{"InitWindow(s1)", "Functions"}));
    EXPECT_EQ(b->ActiveSurface(), kS1);
    b->ClearCalls();
    EXPECT_FALSE(Swap(kS1));
    EXPECT_EQ(TraceWithTableReads(), (Vector<String>{"Functions"}));
    EXPECT_EQ(b->PresentCount(), 0u);
}

// A swap of a pending surface still presents, and changes no binding, pending destroy,
// display or probe flag.
TEST_F(BackendObjectEglSwap, ASwapOfAPendingSurfaceStillPresents) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    b->ReleaseEGLSurface(kS1);
    b->ClearCalls();
    EXPECT_TRUE(Swap(kS1));
    EXPECT_EQ(Trace(), (Vector<String>{"Present"}));
    EXPECT_TRUE(RecordIs(b->Record(kS1), WindowRecord(kWin1, 640, 480, true, true)));
    EXPECT_TRUE(BoundTo(b->CurrentHere(), kDpy, kS1, kCtx));
    EXPECT_EQ(b->DisplayNow(), kDpy);
    EXPECT_TRUE(b->CapsProbed());
}

// The default swap-with-damage ignores the damage and dispatches to the virtual swap.
TEST_F(BackendObjectEglSwap, SwapWithDamageDispatchesToTheVirtualSwap) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    b->ClearCalls();
    const MG_Util::Damage::Rect rect{.X = 1, .Y = 2, .Width = 3, .Height = 4};
    EXPECT_TRUE(b->SwapEGLBuffersWithDamage(kDpy, kS1, MG_Util::Damage::Region::FromRects(&rect, 1)));
    EXPECT_EQ(b->swapOverrideCalls.load(), 1);
    EXPECT_EQ(Trace(), (Vector<String>{"Present"}));
    EXPECT_FALSE(b->SwapEGLBuffersWithDamage(kDpy2, kS1, MG_Util::Damage::Region::Full()));
    EXPECT_EQ(b->swapOverrideCalls.load(), 2);
    EXPECT_EQ(b->PresentCount(), 1u);
}

// ========================================= swap interval =========================================

// The interval is forwarded once, or dropped when the slot is null; no bookkeeping changes.
TEST_F(BackendObjectEglSwapInterval, TheIntervalIsForwardedOnceOrDropped) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    b->ClearCalls();
    b->SetEGLSwapInterval(2);
    EXPECT_EQ(TraceWithTableReads(), (Vector<String>{"Functions", "SwapInterval(2)"}));
    b->SetEGLSwapInterval(0);
    EXPECT_EQ(TraceWithTableReads(), (Vector<String>{"Functions", "SwapInterval(2)", "Functions", "SwapInterval(0)"}));
    EXPECT_EQ(b->SwapIntervalCount(), 2u);
    b->SetSwapIntervalNull(true);
    b->ClearCalls();
    b->SetEGLSwapInterval(3);
    EXPECT_EQ(TraceWithTableReads(), (Vector<String>{"Functions"}));
    EXPECT_EQ(b->SwapIntervalCount(), 2u);
    EXPECT_EQ(b->ActiveSurface(), kS1);
    EXPECT_TRUE(b->CapsProbed());
    EXPECT_TRUE(BoundTo(b->CurrentHere(), kDpy, kS1, kCtx));
}

// =========================================== release ===========================================

// An unknown or already-released surface is a no-op.
TEST_F(BackendObjectEglRelease, ReleasingAnUnknownSurfaceIsANoOp) {
    b->ReleaseEGLSurface(kS9);
    ASSERT_TRUE(Init());
    ASSERT_TRUE(b->CreateEGLWindowSurface(kS1, kWin1));
    b->ClearCalls();
    b->ReleaseEGLSurface(kS9);
    EXPECT_TRUE(Trace().empty());
    b->ReleaseEGLSurface(kS1);
    EXPECT_EQ(Trace(), (Vector<String>{"Released(s1)", "Forgotten(s1)"}));
    b->ClearCalls();
    b->ReleaseEGLSurface(kS1);
    EXPECT_TRUE(Trace().empty());
}

// Releasing a current surface only marks it; it stays usable (swap, resize,
// activation, another thread's bind) until the last thread holding it lets go, on whose thread
// the destroy's hooks run.
TEST_F(BackendObjectEglRelease, ReleasingACurrentSurfaceOnlyMarksIt) {
    Worker other;
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    ASSERT_TRUE(b->RegisterEGLPbufferSurface(kS2, 4, 4));
    b->ClearCalls();
    b->ReleaseEGLSurface(kS1);
    EXPECT_TRUE(Trace().empty());
    EXPECT_TRUE(RecordIs(b->Record(kS1), WindowRecord(kWin1, 640, 480, true, true)));
    EXPECT_EQ(b->ActiveSurface(), kS1);
    EXPECT_TRUE(BoundTo(b->CurrentHere(), kDpy, kS1, kCtx));
    EXPECT_TRUE(b->CapsProbed());
    b->ReleaseEGLSurface(kS1);
    EXPECT_TRUE(Trace().empty());
    EXPECT_TRUE(b->Record(kS1)->DestroyPending);
    EXPECT_TRUE(Swap(kS1));
    EXPECT_TRUE(b->ResizeEGLWindowSurface(kS1, 5, 6));
    EXPECT_TRUE(b->ActivateEGLSurface(kS2));
    EXPECT_TRUE(b->ActivateEGLSurface(kS1));
    EXPECT_TRUE(BindOn(other, kS1, kCtx2));
    EXPECT_EQ(Trace(), (Vector<String>{"Present", "InitPbuffer(s2,4,4)", "InitWindow(s1)", "InitCaps"}));
    b->ClearCalls();
    EXPECT_TRUE(Release());
    EXPECT_TRUE(Trace().empty());
    EXPECT_TRUE(b->IsRegistered(kS1));
    EXPECT_TRUE(ReleaseOn(other));
    EXPECT_EQ(Trace(), (Vector<String>{"Released(s1)", "Forgotten(s1)"}));
    EXPECT_TRUE(AllCallsOn(other.Id()));
    EXPECT_FALSE(b->IsRegistered(kS1));
}

// Releasing the idle active surface drops its record, runs Released (still seeing
// it active), forgets it, then runs Forgotten. Bindings, other records, the probe flag (set
// here), the display and the activating surface are untouched.
TEST_F(BackendObjectEglRelease, ReleasingTheIdleActiveSurfaceRunsTheTriple) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(b->CreateEGLPbufferSurface(kS2, 4, 4));
    ASSERT_TRUE(b->CreateEGLWindowSurface(kS1, kWin1));
    ASSERT_TRUE(Bind(kS2));
    ASSERT_TRUE(b->ActivateEGLSurface(kS1));
    ASSERT_TRUE(b->CapsProbed());
    b->ClearCalls();
    b->ReleaseEGLSurface(kS1);
    EXPECT_EQ(Trace(), (Vector<String>{"Released(s1)", "Forgotten(s1)"}));
    EXPECT_TRUE(AllCallsOn(Self()));
    const auto released = Hooks()[0].State;
    EXPECT_TRUE(released.HasActive);
    EXPECT_EQ(released.Active, kS1);
    EXPECT_EQ(released.Kind, Kind::Window);
    EXPECT_TRUE(WindowIs(released.Window, kWin1));
    EXPECT_FALSE(released.Subject.has_value());
    const auto forgotten = Hooks()[1].State;
    EXPECT_FALSE(forgotten.HasActive);
    EXPECT_EQ(forgotten.Active, EGL_NO_SURFACE);
    EXPECT_EQ(forgotten.Kind, Kind::None);
    EXPECT_TRUE(WindowIs(forgotten.Window, kNoWindow));
    EXPECT_FALSE(forgotten.Subject.has_value());
    EXPECT_FALSE(b->Record(kS1).has_value());
    EXPECT_FALSE(b->HasActiveSurface());
    EXPECT_TRUE(ActiveInvariantHolds(*b));
    EXPECT_TRUE(b->CapsProbed());
    EXPECT_TRUE(BoundTo(b->CurrentHere(), kDpy, kS2, kCtx));
    EXPECT_TRUE(RecordIs(b->Record(kS2), PbufferRecord(4, 4, true)));
    EXPECT_EQ(b->DisplayNow(), kDpy);
    EXPECT_EQ(b->ActivatingSurface(), kS1);
}

// A surface that is not active gets only Forgotten.
TEST_F(BackendObjectEglRelease, ReleasingAnInactiveSurfaceRunsOnlyForgotten) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(b->CreateEGLWindowSurface(kS1, kWin1));
    ASSERT_TRUE(b->RegisterEGLPbufferSurface(kS2, 4, 4));
    b->ClearCalls();
    b->ReleaseEGLSurface(kS2);
    EXPECT_EQ(Trace(), (Vector<String>{"Forgotten(s2)"}));
    EXPECT_TRUE(Hooks()[0].State.HasActive);
    EXPECT_EQ(Hooks()[0].State.Active, kS1);
    EXPECT_FALSE(Hooks()[0].State.Subject.has_value());
    EXPECT_EQ(b->ActiveSurface(), kS1);
    EXPECT_TRUE(WindowIs(b->WindowHandleNow(), kWin1));
}

// Released goes to the ACTIVE surface, not the activating one. After a failed
// activation of s2, releasing s2 runs only Forgotten and releasing s1 runs the whole triple.
TEST_F(BackendObjectEglRelease, ReleasedFollowsTheActiveSurfaceNotTheActivatingOne) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(b->CreateEGLWindowSurface(kS1, kWin1));
    ASSERT_TRUE(b->RegisterEGLWindowSurface(kS2, kWin2));
    b->failWindow = true;
    ASSERT_FALSE(b->ActivateEGLSurface(kS2));
    b->failWindow = false;
    b->ClearCalls();
    b->ReleaseEGLSurface(kS2);
    EXPECT_EQ(Trace(), (Vector<String>{"Forgotten(s2)"}));
    EXPECT_EQ(b->ActiveSurface(), kS1);
    EXPECT_TRUE(WindowIs(b->WindowHandleNow(), kWin2));
    b->ClearCalls();
    b->ReleaseEGLSurface(kS1);
    EXPECT_EQ(Trace(), (Vector<String>{"Released(s1)", "Forgotten(s1)"}));
    EXPECT_FALSE(b->HasActiveSurface());
    EXPECT_EQ(b->ActivatingSurface(), kS2);
}

// A pending active surface stays active until its last binding goes, then the
// same triple runs, after the binding was erased.
TEST_F(BackendObjectEglRelease, APendingActiveSurfaceIsDestroyedAtTheLastRelease) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    b->ClearCalls();
    b->ReleaseEGLSurface(kS1);
    EXPECT_TRUE(Trace().empty());
    EXPECT_EQ(b->ActiveSurface(), kS1);
    EXPECT_TRUE(Release());
    EXPECT_EQ(Trace(), (Vector<String>{"Released(s1)", "Forgotten(s1)"}));
    const auto released = Hooks()[0].State;
    EXPECT_TRUE(released.HasActive);
    EXPECT_EQ(released.Active, kS1);
    EXPECT_TRUE(WindowIs(released.Window, kWin1));
    EXPECT_FALSE(released.Subject.has_value());
    EXPECT_FALSE(released.Current.has_value());
    const auto forgotten = Hooks()[1].State;
    EXPECT_FALSE(forgotten.HasActive);
    EXPECT_EQ(forgotten.Kind, Kind::None);
    EXPECT_TRUE(WindowIs(forgotten.Window, kNoWindow));
    EXPECT_FALSE(b->HasActiveSurface());
    EXPECT_FALSE(b->Record(kS1).has_value());
    EXPECT_TRUE(b->CapsProbed());
}

// The deferred destroy, too, runs Released for the ACTIVE surface only, whatever the
// activating surface names. First the pending s1 is active while a failed activation left s2
// activating; then the pending s2 is the activating surface while s3 is active.
TEST_F(BackendObjectEglRelease, ADeferredDestroyFollowsTheActiveSurfaceNotTheActivatingOne) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    b->ReleaseEGLSurface(kS1);
    ASSERT_TRUE(b->RegisterEGLWindowSurface(kS2, kWin2));
    b->failWindow = true;
    ASSERT_FALSE(b->ActivateEGLSurface(kS2));
    b->failWindow = false;
    b->ClearCalls();
    EXPECT_TRUE(Release());
    EXPECT_EQ(Trace(), (Vector<String>{"Released(s1)", "Forgotten(s1)"}));
    EXPECT_FALSE(b->HasActiveSurface());
    ASSERT_TRUE(Bind(kS2));
    ASSERT_TRUE(b->CreateEGLPbufferSurface(kS3, 4, 4));
    b->ReleaseEGLSurface(kS2);
    b->failWindow = true;
    ASSERT_FALSE(b->ActivateEGLSurface(kS2));
    b->failWindow = false;
    ASSERT_EQ(b->ActivatingSurface(), kS2);
    b->ClearCalls();
    EXPECT_TRUE(Release());
    EXPECT_EQ(Trace(), (Vector<String>{"Forgotten(s2)"}));
    EXPECT_EQ(b->ActiveSurface(), kS3);
    EXPECT_FALSE(b->IsRegistered(kS2));
}

// ============================================ threads ============================================

// Binds never touch another thread's binding; two threads may share a surface and a
// context, and one thread moving the backend to another surface leaves the other's binding.
TEST_F(BackendObjectEglThreads, EachThreadKeepsItsOwnBinding) {
    Worker a;
    Worker c;
    ASSERT_TRUE(Init());
    ASSERT_TRUE(b->CreateEGLWindowSurface(kS1, kWin1));
    ASSERT_TRUE(b->CreateEGLWindowSurface(kS2, kWin2));
    EXPECT_TRUE(BindOn(a, kS1));
    EXPECT_TRUE(BindOn(c, kS1));
    EXPECT_TRUE(BoundTo(CurrentOn(a), kDpy, kS1, kCtx));
    EXPECT_TRUE(BoundTo(CurrentOn(c), kDpy, kS1, kCtx));
    EXPECT_TRUE(BindOn(c, kS2, kCtx2));
    EXPECT_EQ(b->ActiveSurface(), kS2);
    EXPECT_TRUE(BoundTo(CurrentOn(a), kDpy, kS1, kCtx));
    EXPECT_TRUE(BoundTo(CurrentOn(c), kDpy, kS2, kCtx2));
    EXPECT_EQ(b->CurrentCount(), 2u);
    EXPECT_FALSE(b->CurrentHere().has_value());
    EXPECT_TRUE(ReleaseOn(a));
    EXPECT_FALSE(CurrentOn(a).has_value());
    EXPECT_TRUE(BoundTo(CurrentOn(c), kDpy, kS2, kCtx2));
    EXPECT_EQ(b->CurrentCount(), 1u);
}

// A swap needs THIS thread's binding, even while another thread has the surface bound;
// Present runs on the swapping thread.
TEST_F(BackendObjectEglThreads, ASwapNeedsThisThreadsBinding) {
    Worker a;
    ASSERT_TRUE(Init());
    ASSERT_TRUE(b->CreateEGLWindowSurface(kS1, kWin1));
    ASSERT_TRUE(BindOn(a, kS1));
    b->ClearCalls();
    EXPECT_FALSE(Swap(kS1));
    EXPECT_TRUE(TraceWithTableReads().empty());
    EXPECT_EQ(b->PresentCount(), 0u);
    EXPECT_TRUE(SwapOn(a, kS1));
    EXPECT_EQ(Trace(), (Vector<String>{"Present"}));
    EXPECT_TRUE(AllCallsOn(a.Id()));
    EXPECT_EQ(b->PresentCount(), 1u);
}

// After thread C moved the backend to its window, thread A's swap brings A's
// window back (its init hook again, no probe), presents once, and C's binding stays.
TEST_F(BackendObjectEglThreads, ASwapBringsBackThisThreadsWindow) {
    Worker a;
    Worker c;
    ASSERT_TRUE(Init());
    ASSERT_TRUE(b->CreateEGLWindowSurface(kS1, kWin1));
    ASSERT_TRUE(b->CreateEGLWindowSurface(kS2, kWin2));
    ASSERT_TRUE(BindOn(a, kS1));
    ASSERT_TRUE(BindOn(c, kS2, kCtx2));
    b->ClearCalls();
    EXPECT_TRUE(SwapOn(a, kS1));
    EXPECT_EQ(Trace(), (Vector<String>{"InitWindow(s1)", "Present"}));
    EXPECT_TRUE(AllCallsOn(a.Id()));
    EXPECT_EQ(Hooks()[0].State.Activating, kS1);
    EXPECT_TRUE(WindowIs(Hooks()[0].State.Window, kWin1));
    EXPECT_TRUE(BoundTo(CurrentOn(c), kDpy, kS2, kCtx2));
    b->ClearCalls();
    EXPECT_TRUE(SwapOn(c, kS2));
    EXPECT_EQ(Trace(), (Vector<String>{"InitWindow(s2)", "Present"}));
    EXPECT_TRUE(AllCallsOn(c.Id()));
    EXPECT_EQ(b->PresentCount(), 2u);
    EXPECT_TRUE(BoundTo(CurrentOn(a), kDpy, kS1, kCtx));
}

// A surface current on two threads is destroyed when the LAST of them lets go, and
// its hooks run on that thread.
TEST_F(BackendObjectEglThreads, APendingSurfaceDiesWithItsLastHolder) {
    Worker a;
    Worker c;
    ASSERT_TRUE(Init());
    ASSERT_TRUE(b->CreateEGLWindowSurface(kS1, kWin1));
    ASSERT_TRUE(BindOn(a, kS1));
    ASSERT_TRUE(BindOn(c, kS1, kCtx2));
    b->ClearCalls();
    b->ReleaseEGLSurface(kS1);
    EXPECT_TRUE(Trace().empty());
    EXPECT_TRUE(ReleaseOn(a));
    EXPECT_TRUE(Trace().empty());
    EXPECT_TRUE(b->IsRegistered(kS1));
    EXPECT_EQ(b->ActiveSurface(), kS1);
    EXPECT_TRUE(ReleaseOn(c));
    EXPECT_EQ(Trace(), (Vector<String>{"Released(s1)", "Forgotten(s1)"}));
    EXPECT_TRUE(AllCallsOn(c.Id()));
    EXPECT_FALSE(b->HasActiveSurface());
    EXPECT_FALSE(b->IsRegistered(kS1));
}

// Or at the holder's next bind onto another surface; another surface being active
// by then, only Forgotten runs.
TEST_F(BackendObjectEglThreads, APendingSurfaceDiesAtItsHoldersNextBind) {
    Worker a;
    ASSERT_TRUE(Init());
    ASSERT_TRUE(b->CreateEGLWindowSurface(kS1, kWin1));
    ASSERT_TRUE(BindOn(a, kS1));
    ASSERT_TRUE(b->CreateEGLPbufferSurface(kS2, 4, 4));
    b->ReleaseEGLSurface(kS1);
    b->ClearCalls();
    EXPECT_TRUE(BindOn(a, kS2));
    EXPECT_EQ(Trace(), (Vector<String>{"InitCaps", "Forgotten(s1)"}));
    EXPECT_TRUE(AllCallsOn(a.Id()));
    EXPECT_EQ(b->ActiveSurface(), kS2);
    EXPECT_FALSE(b->IsRegistered(kS1));
}

// Once the active surface died, other threads' swaps re-activate their own surfaces and a
// bind activates its surface lazily.
TEST_F(BackendObjectEglThreads, AfterTheActiveSurfaceDiesEachThreadBringsItsOwnBack) {
    Worker a;
    ASSERT_TRUE(Init());
    ASSERT_TRUE(b->CreateEGLWindowSurface(kS1, kWin1));
    ASSERT_TRUE(b->CreateEGLWindowSurface(kS2, kWin2));
    ASSERT_TRUE(BindOn(a, kS1));
    ASSERT_TRUE(Bind(kS2, kCtx2));
    ASSERT_TRUE(Release());
    b->ClearCalls();
    b->ReleaseEGLSurface(kS2);
    EXPECT_EQ(Trace(), (Vector<String>{"Released(s2)", "Forgotten(s2)"}));
    EXPECT_FALSE(b->HasActiveSurface());
    b->ClearCalls();
    EXPECT_TRUE(SwapOn(a, kS1));
    EXPECT_EQ(Trace(), (Vector<String>{"InitWindow(s1)", "Present"}));
    EXPECT_TRUE(AllCallsOn(a.Id()));
    EXPECT_EQ(b->ActiveSurface(), kS1);
    ASSERT_TRUE(ReleaseOn(a));
    b->ReleaseEGLSurface(kS1);
    ASSERT_FALSE(b->HasActiveSurface());
    ASSERT_TRUE(b->RegisterEGLPbufferSurface(kS3, 2, 2));
    b->ClearCalls();
    EXPECT_TRUE(Bind(kS3));
    EXPECT_EQ(Trace(), (Vector<String>{"InitPbuffer(s3,2,2)", "InitCaps"}));
}

// ===================================== reset, forget, teardown =====================================

// A reset drops the active surface, the probe flag, the window handle and EVERY
// thread's binding, keeps the records (Activated, DestroyPending), the display and the activating
// surface, and runs no hook. A pending record dies only at a later release that finds it idle.
TEST_F(BackendObjectEglReset, AResetDropsRuntimeStateAndKeepsRecords) {
    Worker a;
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    ASSERT_TRUE(BindOn(a, kS1, kCtx2));
    ASSERT_TRUE(b->RegisterEGLPbufferSurface(kS2, 4, 4));
    b->ReleaseEGLSurface(kS1);
    b->ClearCalls();
    b->ResetEGLRuntimeState();
    EXPECT_TRUE(Trace().empty());
    EXPECT_FALSE(b->HasActiveSurface());
    EXPECT_EQ(b->ActiveSurface(), EGL_NO_SURFACE);
    EXPECT_EQ(b->ActiveKind(), Kind::None);
    EXPECT_TRUE(WindowIs(b->WindowHandleNow(), kNoWindow));
    EXPECT_FALSE(b->CapsProbed());
    EXPECT_EQ(b->CurrentCount(), 0u);
    EXPECT_FALSE(b->CurrentHere().has_value());
    EXPECT_FALSE(CurrentOn(a).has_value());
    EXPECT_TRUE(RecordIs(b->Record(kS1), WindowRecord(kWin1, 640, 480, true, true)));
    EXPECT_TRUE(RecordIs(b->Record(kS2), PbufferRecord(4, 4)));
    EXPECT_TRUE(b->DisplayInitialized());
    EXPECT_EQ(b->DisplayNow(), kDpy);
    EXPECT_EQ(b->ActivatingSurface(), kS1);
    EXPECT_TRUE(ReleaseOn(a));
    EXPECT_TRUE(Release());
    EXPECT_TRUE(Trace().empty());
    EXPECT_TRUE(b->IsRegistered(kS1));
    b->ReleaseEGLSurface(kS1);
    EXPECT_EQ(Trace(), (Vector<String>{"Forgotten(s1)"}));
}

// After a reset every swap fails until its thread binds again, and that bind probes.
TEST_F(BackendObjectEglReset, AfterAResetSwapsFailUntilTheNextBind) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    b->ResetEGLRuntimeState();
    b->ClearCalls();
    EXPECT_FALSE(Swap(kS1));
    EXPECT_TRUE(TraceWithTableReads().empty());
    EXPECT_TRUE(Bind(kS1));
    EXPECT_EQ(Trace(), (Vector<String>{"InitWindow(s1)", "InitCaps"}));
    EXPECT_TRUE(Swap(kS1));
}

// A forget drops only the active surface, its kind and the window handle; the probe
// flag, every binding, the records, the display and the activating surface stay.
TEST_F(BackendObjectEglReset, AForgetKeepsTheProbeFlagBindingsAndRecords) {
    Worker a;
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    ASSERT_TRUE(BindOn(a, kS1, kCtx2));
    b->ClearCalls();
    b->ForgetActiveEGLSurface();
    EXPECT_TRUE(Trace().empty());
    EXPECT_FALSE(b->HasActiveSurface());
    EXPECT_EQ(b->ActiveSurface(), EGL_NO_SURFACE);
    EXPECT_EQ(b->ActiveKind(), Kind::None);
    EXPECT_TRUE(WindowIs(b->WindowHandleNow(), kNoWindow));
    EXPECT_TRUE(b->CapsProbed());
    EXPECT_TRUE(BoundTo(b->CurrentHere(), kDpy, kS1, kCtx));
    EXPECT_TRUE(BoundTo(CurrentOn(a), kDpy, kS1, kCtx2));
    EXPECT_TRUE(RecordIs(b->Record(kS1), WindowRecord(kWin1, 640, 480, true)));
    EXPECT_EQ(b->DisplayNow(), kDpy);
    EXPECT_TRUE(b->DisplayInitialized());
    EXPECT_EQ(b->ActivatingSurface(), kS1);
    EXPECT_TRUE(Bind(kS1));
    EXPECT_EQ(Trace(), (Vector<String>{"InitWindow(s1)"}));
}

// ReleaseEGLResources drops records, bindings, the active surface and the display with
// no hook for any record (active, pending or idle); it is idempotent and leaves the activating
// surface and the format-capability cache alone.
TEST_F(BackendObjectEglReset, ReleasingResourcesDropsEverythingWithoutAHook) {
    Worker a;
    b->MarkFormatCapabilities();
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    ASSERT_TRUE(BindOn(a, kS1, kCtx2));
    ASSERT_TRUE(b->CreateEGLPbufferSurface(kS2, 4, 4));
    ASSERT_TRUE(b->RegisterEGLWindowSurface(kS3, kWin3));
    b->ReleaseEGLSurface(kS1);
    b->ClearCalls();
    for (int round = 0; round < 2; ++round) {
        b->ReleaseEGLResources();
        EXPECT_TRUE(Trace().empty());
        EXPECT_TRUE(NoKnownRecord());
        EXPECT_EQ(b->CurrentCount(), 0u);
        EXPECT_FALSE(b->HasActiveSurface());
        EXPECT_EQ(b->ActiveKind(), Kind::None);
        EXPECT_TRUE(WindowIs(b->WindowHandleNow(), kNoWindow));
        EXPECT_FALSE(b->CapsProbed());
        EXPECT_FALSE(b->DisplayInitialized());
        EXPECT_EQ(b->DisplayNow(), EGL_NO_DISPLAY);
        EXPECT_EQ(b->ActivatingSurface(), kS2);
        EXPECT_TRUE(b->FormatCapabilitiesMarked());
    }
    EXPECT_TRUE(ReleaseOn(a));
    EXPECT_TRUE(Trace().empty());
}

// Afterwards only a release request succeeds until a display is initialized again.
TEST_F(BackendObjectEglReset, AfterReleasingResourcesOnlyAReleaseRequestSucceeds) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(BoundWindow(kS1, kWin1));
    b->ReleaseEGLResources();
    b->ClearCalls();
    EXPECT_FALSE(b->RegisterEGLWindowSurface(kS2, kWin2));
    EXPECT_FALSE(b->RegisterEGLPbufferSurface(kS2, 4, 4));
    EXPECT_FALSE(b->CreateEGLWindowSurface(kS2, kWin2));
    EXPECT_FALSE(b->CreateEGLPbufferSurface(kS2, 4, 4));
    EXPECT_FALSE(Bind(kS1));
    EXPECT_FALSE(b->MakeEGLCurrent(EGL_NO_DISPLAY, kS1, kS1, kCtx));
    EXPECT_FALSE(Swap(kS1));
    EXPECT_TRUE(Release());
    b->ReleaseEGLSurface(kS1);
    EXPECT_TRUE(TraceWithTableReads().empty());
    EXPECT_TRUE(NoKnownRecord());
    ASSERT_TRUE(Init());
    EXPECT_TRUE(b->CreateEGLWindowSurface(kS1, kWin1));
    EXPECT_EQ(Trace(), (Vector<String>{"InitWindow(s1)"}));
}

// ========================================= window handle =========================================

// SetWindowHandle stores any handle verbatim; a window activation overwrites it, a pbuffer
// activation does not.
TEST_F(BackendObjectEglWindowHandle, SetWindowHandleStoresAnyHandleVerbatim) {
    b->SetWindowHandle(Win(0, 3, 4, WindowBackend::Unknown));
    EXPECT_TRUE(WindowIs(b->WindowHandleNow(), Win(0, 3, 4, WindowBackend::Unknown)));
    ASSERT_TRUE(Init());
    ASSERT_TRUE(b->CreateEGLWindowSurface(kS1, kWin1));
    ASSERT_TRUE(b->RegisterEGLPbufferSurface(kS2, 4, 4));
    b->ClearCalls();
    b->SetWindowHandle(kWin3);
    EXPECT_TRUE(WindowIs(b->WindowHandleNow(), kWin3));
    EXPECT_EQ(b->ActiveSurface(), kS1);
    EXPECT_TRUE(RecordIs(b->Record(kS1), WindowRecord(kWin1, 640, 480, true)));
    EXPECT_TRUE(Trace().empty());
    ASSERT_TRUE(b->ActivateEGLSurface(kS2));
    EXPECT_TRUE(WindowIs(b->WindowHandleNow(), kWin3));
    ASSERT_TRUE(b->ActivateEGLSurface(kS1));
    EXPECT_TRUE(WindowIs(b->WindowHandleNow(), kWin1));
}

// ============================================ locking ============================================

// Hooks may call back into the base on their own thread (the state lock is recursive).
TEST_F(BackendObjectEglLocking, AHookMayCallBackIntoTheBase) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(b->RegisterEGLWindowSurface(kS1, kWin1));
    ASSERT_TRUE(b->RegisterEGLPbufferSurface(kS2, 4, 4));
    std::atomic<Int> reentered{0};
    b->onHook = [this, &reentered](const EglDouble::Call& call) {
        switch (call.What) {
        case Hook::InitWindow:
            EXPECT_TRUE(b->IsRegistered(kS1));
            b->SetWindowHandle(b->WindowHandleNow());
            ++reentered;
            break;
        case Hook::InitCaps:
            EXPECT_TRUE(b->InitializeEGLDisplay(kDpy, nullptr, nullptr));
            EXPECT_TRUE(b->ResizeEGLWindowSurface(kS2, 4, 4));
            EXPECT_FALSE(b->MakeEGLCurrent(kDpy2, kS1, kS1, kCtx));
            b->ReleaseEGLSurface(kS9);
            ++reentered;
            break;
        case Hook::Present:
            EXPECT_FALSE(b->SwapEGLBuffers(kDpy2, kS1));
            ++reentered;
            break;
        case Hook::Forgotten:
            EXPECT_FALSE(b->IsRegistered(call.Surface));
            EXPECT_TRUE(Release());
            ++reentered;
            break;
        default:
            break;
        }
    };
    EXPECT_TRUE(BindOn(holder, kS1));
    EXPECT_TRUE(SwapOn(holder, kS1));
    holder.Run([this] { b->ReleaseEGLSurface(kS1); });
    EXPECT_TRUE(ReleaseOn(holder));
    b->onHook = nullptr;
    EXPECT_EQ(reentered.load(), 4);
    EXPECT_EQ(Trace(), (Vector<String>{"InitWindow(s1)", "InitCaps", "Present", "Released(s1)", "Forgotten(s1)"}));
    EXPECT_TRUE(AllCallsOn(holder.Id()));
}

// While one thread is inside a hook, every locking base method called on another thread
// waits for it.
TEST_F(BackendObjectEglLocking, ABaseCallWaitsWhileAnotherThreadIsInAHook) {
    struct Probe {
        const char* Name;
        std::function<void(EglDouble&)> Call;
    };
    const Vector<Probe> probes = {
        {"InitializeEGLDisplay", [](EglDouble& d) { (void)d.InitializeEGLDisplay(kDpy, nullptr, nullptr); }},
        {"RegisterEGLWindowSurface", [](EglDouble& d) { (void)d.RegisterEGLWindowSurface(kS3, kWin3); }},
        {"RegisterEGLPbufferSurface", [](EglDouble& d) { (void)d.RegisterEGLPbufferSurface(kS3, 2, 2); }},
        {"CreateEGLWindowSurface", [](EglDouble& d) { (void)d.CreateEGLWindowSurface(kS3, kWin3); }},
        {"CreateEGLPbufferSurface", [](EglDouble& d) { (void)d.CreateEGLPbufferSurface(kS3, 2, 2); }},
        {"GetRegisteredEGLSurface", [](EglDouble& d) { (void)d.IsRegistered(kS1); }},
        {"ActivateEGLSurface", [](EglDouble& d) { (void)d.ActivateEGLSurface(kS2); }},
        {"ResizeEGLWindowSurface", [](EglDouble& d) { (void)d.ResizeEGLWindowSurface(kS1, 2, 2); }},
        {"MakeEGLCurrent",
         [](EglDouble& d) { (void)d.MakeEGLCurrent(EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); }},
        {"SwapEGLBuffers", [](EglDouble& d) { (void)d.SwapEGLBuffers(kDpy, kS1); }},
        {"ReleaseEGLSurface", [](EglDouble& d) { d.ReleaseEGLSurface(kS9); }},
        {"ReleaseEGLResources", [](EglDouble& d) { d.ReleaseEGLResources(); }},
        {"ResetEGLRuntimeState", [](EglDouble& d) { d.ResetEGLRuntimeState(); }},
        {"ForgetActiveEGLSurface", [](EglDouble& d) { d.ForgetActiveEGLSurface(); }},
        {"SetWindowHandle", [](EglDouble& d) { d.SetWindowHandle(kWin3); }},
    };
    for (const auto& probe : probes) {
        SCOPED_TRACE(probe.Name);
        Fresh();
        ASSERT_TRUE(Init());
        ASSERT_TRUE(b->RegisterEGLWindowSurface(kS1, kWin1));
        ASSERT_TRUE(b->RegisterEGLPbufferSurface(kS2, 4, 4));
        const ProbeOutcome outcome = ProbeDuringHook(
            Hook::InitCaps, [this] { EXPECT_TRUE(Bind(kS1)); }, [this, &probe](Latch&) { probe.Call(*b); },
            kBlockWindow);
        EXPECT_EQ(outcome, ProbeOutcome::Blocked) << probe.Name << " while another thread was in InitCapabilities";
    }
}

// Every hook, Present included, runs with the state lock held by its caller, whichever
// entry point reached it.
TEST_F(BackendObjectEglLocking, EveryHookRunsUnderTheStateLock) {
    // A setup gets the double and the holding thread, which some scenarios bind first.
    using Setup = std::function<void(EglDouble&, Worker&)>;
    struct Scenario {
        const char* Name;
        Hook Held;
        Setup Prepare;
        std::function<void(EglDouble&)> Hold;
    };
    const Setup none = [](EglDouble&, Worker&) {};
    const Setup window = [](EglDouble& d, Worker&) { ASSERT_TRUE(d.RegisterEGLWindowSurface(kS1, kWin1)); };
    const Setup pbuffer = [](EglDouble& d, Worker&) { ASSERT_TRUE(d.RegisterEGLPbufferSurface(kS2, 4, 4)); };
    const Setup created = [](EglDouble& d, Worker&) { ASSERT_TRUE(d.CreateEGLWindowSurface(kS1, kWin1)); };
    const Setup heldBound = [](EglDouble& d, Worker& w) {
        ASSERT_TRUE(d.CreateEGLWindowSurface(kS1, kWin1));
        ASSERT_TRUE(w.Run([&d] { return d.MakeEGLCurrent(kDpy, kS1, kS1, kCtx); }));
    };
    const Setup heldBoundElsewhere = [heldBound](EglDouble& d, Worker& w) {
        heldBound(d, w);
        ASSERT_TRUE(d.CreateEGLPbufferSurface(kS2, 4, 4));
    };
    const Setup heldPending = [heldBound](EglDouble& d, Worker& w) {
        heldBound(d, w);
        d.ReleaseEGLSurface(kS1);
    };
    const auto release = [](EglDouble& d) {
        (void)d.MakeEGLCurrent(EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    };
    const Vector<Scenario> scenarios = {
        {"CreateEGLWindowSurface/InitWindowSurface", Hook::InitWindow, none,
         [](EglDouble& d) { (void)d.CreateEGLWindowSurface(kS1, kWin1); }},
        {"CreateEGLPbufferSurface/InitPbufferSurface", Hook::InitPbuffer, none,
         [](EglDouble& d) { (void)d.CreateEGLPbufferSurface(kS2, 4, 4); }},
        {"ActivateEGLSurface/InitWindowSurface", Hook::InitWindow, window,
         [](EglDouble& d) { (void)d.ActivateEGLSurface(kS1); }},
        {"ActivateEGLSurface/InitPbufferSurface", Hook::InitPbuffer, pbuffer,
         [](EglDouble& d) { (void)d.ActivateEGLSurface(kS2); }},
        {"MakeEGLCurrent/InitWindowSurface", Hook::InitWindow, window,
         [](EglDouble& d) { (void)d.MakeEGLCurrent(kDpy, kS1, kS1, kCtx); }},
        {"MakeEGLCurrent/InitCapabilities", Hook::InitCaps, created,
         [](EglDouble& d) { (void)d.MakeEGLCurrent(kDpy, kS1, kS1, kCtx); }},
        {"SwapEGLBuffers/Present", Hook::Present, heldBound, [](EglDouble& d) { (void)d.SwapEGLBuffers(kDpy, kS1); }},
        {"SwapEGLBuffers/InitWindowSurface", Hook::InitWindow, heldBoundElsewhere,
         [](EglDouble& d) { (void)d.SwapEGLBuffers(kDpy, kS1); }},
        {"ReleaseEGLSurface/OnEGLSurfaceReleased", Hook::Released, created,
         [](EglDouble& d) { d.ReleaseEGLSurface(kS1); }},
        {"ReleaseEGLSurface/OnEGLSurfaceForgotten", Hook::Forgotten, created,
         [](EglDouble& d) { d.ReleaseEGLSurface(kS1); }},
        {"MakeEGLCurrent release/OnEGLSurfaceReleased", Hook::Released, heldPending, release},
        {"MakeEGLCurrent release/OnEGLSurfaceForgotten", Hook::Forgotten, heldPending, release},
    };
    for (const auto& scenario : scenarios) {
        SCOPED_TRACE(scenario.Name);
        Fresh();
        ASSERT_TRUE(Init());
        scenario.Prepare(*b, holder);
        ASSERT_FALSE(HasFatalFailure());
        const ProbeOutcome outcome = ProbeDuringHook(
            scenario.Held, [this, &scenario] { scenario.Hold(*b); }, [this](Latch&) { (void)b->IsRegistered(kS1); },
            kBlockWindow);
        EXPECT_EQ(outcome, ProbeOutcome::Blocked) << "a lookup while another thread was inside " << scenario.Name;
    }
}

// SetEGLSwapInterval and the default SwapEGLBuffersWithDamage take no lock of their
// own: both get through while another thread is inside a hook.
TEST_F(BackendObjectEglLocking, SwapIntervalAndSwapWithDamageTakeNoLock) {
    ASSERT_TRUE(Init());
    ASSERT_TRUE(b->RegisterEGLWindowSurface(kS1, kWin1));
    EXPECT_EQ(ProbeDuringHook(
                  Hook::InitCaps, [this] { EXPECT_TRUE(Bind(kS1)); }, [this](Latch&) { b->SetEGLSwapInterval(1); },
                  kReachWindow),
              ProbeOutcome::GotThrough);
    EXPECT_EQ(b->SwapIntervalCount(), 1u);
    Fresh();
    ASSERT_TRUE(Init());
    ASSERT_TRUE(b->RegisterEGLWindowSurface(kS1, kWin1));
    EXPECT_EQ(ProbeDuringHook(
                  Hook::InitCaps, [this] { EXPECT_TRUE(Bind(kS1)); },
                  [this](Latch& reached) {
                      b->onSwapOverride = [&reached] { reached.Set(); };
                      (void)b->SwapEGLBuffersWithDamage(kDpy, kS1, MG_Util::Damage::Region::Full());
                      b->onSwapOverride = nullptr;
                  },
                  kReachWindow),
              ProbeOutcome::GotThrough);
}
