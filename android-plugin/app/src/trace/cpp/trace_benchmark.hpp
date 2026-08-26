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
    // Begin() to End(), in seconds. Covers trace parsing and the leading partial frame too,
    // which is why it is reported next to the per-frame statistics rather than derived from
    // them.
    double totalSeconds = 0.0;
};

// Disarms timing and hands back what was recorded.
Report End();

} // namespace benchmark
} // namespace mobilegl_trace
