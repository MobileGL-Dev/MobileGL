#pragma once

#include <vector>

namespace mobilegl_trace {
namespace benchmark {

// Per-frame wall-clock timing for the retrace loop, shared by the Android replay runner
// and the desktop CLI. Disarmed unless Begin() armed it, and the frame-boundary hook is a
// single bool test in that case, so the correctness harness pays nothing for it.
//
// Retrace runs --singlethread, so all of this is deliberately plain globals: Begin(),
// OnFrameBoundary() and End() are only ever reached from the one retrace thread.
//
// That single-threadedness is also what makes the SECOND series below sound. Beside the wall
// clock, every frame boundary reads CLOCK_THREAD_CPUTIME_ID - the CPU time consumed by THIS
// thread - and because the retrace loop is the only thread that ever gets here, that number is
// the client-side CPU cost of the frame and nothing else. It is the metric the disaggregation
// GO/NO-GO hangs on (ROADMAP.md: per-thread CPU p50/p99, not wall time), and before P2 the tree
// had no first-party collector for it at all: no CLOCK_THREAD_CPUTIME_ID, no getrusage, no
// /proc/self/task anywhere under MobileGL/, tools/, android-plugin/ or scripts/. Collecting it
// here costs one extra clock_gettime per frame, needs no root, no profiler, no debuggable build
// and no sampling, and - unlike a timer inside the library - commits no instrumentation to a hot
// path.
//
// Wall time and CPU time answer different questions and both are kept: with --benchmark-no-finish
// the wall series still contains everything the thread WAITED for (driver submit, the compositor,
// a fence), while the CPU series contains only what it EXECUTED. A change that moves work off the
// retrace thread shows up as the two series diverging, which is exactly the confusion a single
// number invites.

// Arms timing for the retrace that is about to run.
//
// finishEachFrame issues a full glFinish through the replayed context at every frame
// boundary, so a recorded frame time covers GPU completion and not just CPU submission.
// That matters on tiled mobile GPUs, where a swap without a sync returns long before the
// tiler is done and the numbers degenerate into "how fast can we feed the driver". The
// price is that finishing every frame serializes CPU/GPU overlap, so the absolute frame
// times are pessimistic against a real running game - they are deterministic and
// comparable between backends and revisions, which is what a benchmark fixture is for.
// With finishEachFrame off the run measures CPU-side submission only.
void Begin(bool finishEachFrame);

// Frame-boundary hook. Called from the platform glws swapBuffers override, which is where
// apitrace's replay loop advances the frame: retrace_eglSwapBuffers() calls
// frame_complete() and then Drawable::swapBuffers().
void OnFrameBoundary();

struct Report {
    // Wall time of every completed frame, in milliseconds.
    std::vector<double> frameMs;
    // Thread CPU time of every completed frame, in milliseconds, in the SAME ORDER and with the
    // same length as frameMs - the two are pushed together at one frame boundary, so index i is
    // one frame in both. Empty when the platform has no CLOCK_THREAD_CPUTIME_ID, which is the
    // one honest reading of "this run collected no CPU series"; a vector of zeroes would be
    // indistinguishable from a frame that genuinely burned no CPU.
    std::vector<double> frameCpuMs;
    // Begin() to End(), in seconds. Covers trace parsing and the leading partial frame too,
    // which is why it is reported next to the per-frame statistics rather than derived from
    // them.
    double totalSeconds = 0.0;
};

// Disarms timing and hands back what was recorded.
Report End();

} // namespace benchmark
} // namespace mobilegl_trace
