#include "trace_benchmark.hpp"

#include <chrono>
#include <cstdlib>
#include <utility>
#include <dlfcn.h>

// CLOCK_THREAD_CPUTIME_ID is POSIX and present on Linux and on every Android API this replays
// on; the guard exists so the desktop CLI still builds where it is not, and so that "no CPU
// series" is a compile-time fact rather than a silently-zero column.
//
// <time.h>, not <ctime>: clock_gettime, CLOCK_THREAD_CPUTIME_ID and struct timespec are POSIX
// names, and only <time.h> is required to put them at global scope - <ctime> guarantees the C++
// subset in namespace std and leaves the rest to the implementation. glibc and bionic both happen
// to provide them either way; this file is built for both by two different toolchains, so it asks
// for the header that actually promises what it uses.
#if defined(__unix__) || defined(__linux__) || defined(__ANDROID__) || defined(__APPLE__)
#include <time.h>
#define MOBILEGL_TRACE_HAVE_THREAD_CPU_CLOCK 1
#else
#define MOBILEGL_TRACE_HAVE_THREAD_CPU_CLOCK 0
#endif

namespace mobilegl_trace {
namespace benchmark {
namespace {

using Clock = std::chrono::steady_clock;
using GlFinishFn = void (*)();

constexpr std::size_t kFrameReserve = 4096;

bool gEnabled = false;
bool gFinishEachFrame = false;
bool gResolvedGlFinish = false;
GlFinishFn gGlFinish = nullptr;
Clock::time_point gStart;
Clock::time_point gLastBoundary;
std::vector<double> gFrameMs;
std::vector<double> gFrameCpuMs;
double gLastBoundaryCpuMs = 0.0;

// Milliseconds of CPU time this thread has consumed, or -1 where the clock does not exist.
// Negative once means negative always, so End() reports an EMPTY cpu series rather than a
// column of zeroes.
double ThreadCpuMs() {
#if MOBILEGL_TRACE_HAVE_THREAD_CPU_CLOCK
    struct timespec now;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) != 0) {
        return -1.0;
    }
    return static_cast<double>(now.tv_sec) * 1000.0 + static_cast<double>(now.tv_nsec) / 1e6;
#else
    return -1.0;
#endif
}

// Same resolution order the glws layers use for MobileGL's entry points: the replay driver
// already dlopen()ed the library with RTLD_GLOBAL before retrace started, so RTLD_NOLOAD
// finds that handle instead of loading a second copy, and RTLD_DEFAULT is the fallback for
// the case where it was linked in rather than dlopen()ed.
GlFinishFn ResolveGlFinish() {
    void *handle = nullptr;
    const char *library = std::getenv("MOBILEGL_TRACE_LIBRARY");
    if (library != nullptr && library[0] != '\0') {
        handle = dlopen(library, RTLD_NOW | RTLD_GLOBAL | RTLD_NOLOAD);
    }
    if (handle == nullptr) {
        handle = dlopen("libMobileGL.so", RTLD_NOW | RTLD_GLOBAL | RTLD_NOLOAD);
    }
    if (handle != nullptr) {
        void *symbol = dlsym(handle, "glFinish");
        if (symbol != nullptr) {
            return reinterpret_cast<GlFinishFn>(symbol);
        }
    }
    return reinterpret_cast<GlFinishFn>(dlsym(RTLD_DEFAULT, "glFinish"));
}

} // namespace

void Begin(bool finishEachFrame) {
    gFrameMs.clear();
    gFrameMs.reserve(kFrameReserve);
    gFrameCpuMs.clear();
    gFrameCpuMs.reserve(kFrameReserve);
    gFinishEachFrame = finishEachFrame;
    gResolvedGlFinish = false;
    gGlFinish = nullptr;
    // Wall baseline FIRST, CPU baseline second - the same order OnFrameBoundary reads them in,
    // and for the same reason. Frame 0's CPU interval then sits strictly inside its wall interval,
    // so whatever this function costs between the two readings lands in the wall number where it
    // can be seen, instead of inflating the CPU number where it cannot. Taken the other way round
    // (as this was), frame 0 alone reported a CPU delta biased upward against its own wall delta.
    gStart = Clock::now();
    gLastBoundary = gStart;
    gLastBoundaryCpuMs = ThreadCpuMs();
    gEnabled = true;
}

void OnFrameBoundary() {
    if (!gEnabled) {
        return;
    }
    if (gFinishEachFrame) {
        // Resolved on the first boundary rather than in Begin(): a context only exists once
        // the trace has created one, and glFinish before that would be pointless anyway.
        if (!gResolvedGlFinish) {
            gGlFinish = ResolveGlFinish();
            gResolvedGlFinish = true;
        }
        if (gGlFinish != nullptr) {
            gGlFinish();
        }
    }
    // The CPU reading is taken FIRST and the wall reading second, so the wall delta contains the
    // cost of the extra syscall rather than the CPU delta hiding inside it: an inflated wall
    // number is visible, a deflated CPU number is not.
    const double cpuNow = ThreadCpuMs();
    const Clock::time_point now = Clock::now();
    gFrameMs.push_back(std::chrono::duration<double, std::milli>(now - gLastBoundary).count());
    if (cpuNow >= 0.0 && gLastBoundaryCpuMs >= 0.0) {
        gFrameCpuMs.push_back(cpuNow - gLastBoundaryCpuMs);
    }
    gLastBoundaryCpuMs = cpuNow;
    gLastBoundary = now;
}

Report End() {
    Report report;
    if (!gEnabled) {
        return report;
    }
    gEnabled = false;
    gFinishEachFrame = false;
    report.totalSeconds = std::chrono::duration<double>(Clock::now() - gStart).count();
    report.frameMs = std::move(gFrameMs);
    gFrameMs.clear();
    // Only hand back a CPU series that lines up frame-for-frame with the wall series. A short
    // one would be a clock that started failing mid-run, and silently re-indexing it against
    // frameMs would put frame N's wall time next to frame N+k's CPU time.
    if (gFrameCpuMs.size() == report.frameMs.size()) {
        report.frameCpuMs = std::move(gFrameCpuMs);
    }
    gFrameCpuMs.clear();
    return report;
}

} // namespace benchmark
} // namespace mobilegl_trace
