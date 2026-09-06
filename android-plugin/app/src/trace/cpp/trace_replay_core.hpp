#pragma once

#include <string>
#include <vector>

namespace mobilegl_trace {

enum StatusCode {
    STATUS_OK = 0,
    STATUS_INVALID_ARGUMENT = 1,
    STATUS_IO_ERROR = 2,
    STATUS_MOBILEGL_LOAD_ERROR = 3,
    STATUS_RETRACE_NOT_LINKED = 4,
    STATUS_RETRACE_FAILED = 5,
    STATUS_COMPARE_FAILED = 6,
};

constexpr int kDefaultBenchmarkTailFrames = 200;

struct Request {
    std::string tracePath;
    std::string goldenPath;
    std::vector<std::string> alternateGoldenPaths;
    std::string outputDir;
    std::string diffPath;
    std::string backend;
    std::string mobileGlLibrary = "libMobileGL.so";
    std::string angleVariant;
    // Framebuffer-attachment dump points, each `CALL:DIR[:FBO,FBO,...]`. Debug-only; the
    // replay behaves exactly as before when this is empty.
    std::vector<std::string> fboAttachmentDumps;
    // Named GL_TEXTURE_2D dump points, each `CALL,TEXTURE,LEVEL,DIR`. Debug-only; the replay
    // behaves exactly as before when this is empty.
    std::vector<std::string> texture2dDumps;
    // Benchmark (frame-timing) mode. Off by default. When on, the replay runs the whole
    // trace from start to finish and records a wall-clock timestamp at every frame
    // boundary; no snapshot is taken and no golden comparison runs.
    bool benchmark = false;
    // Number of trailing frames the summary statistics are computed over. Clamped to the
    // number of frames actually recorded. The tail is what is comparable between runs: the
    // head of a trace is dominated by shader compiles and first-use uploads.
    int benchmarkTailFrames = kDefaultBenchmarkTailFrames;
    // glFinish through the replayed context at every frame boundary, so a frame time
    // includes GPU completion instead of only CPU submission. See trace_benchmark.hpp.
    bool benchmarkFinish = true;
    // Where the timing JSON goes. Defaults to <outputDir>/benchmark.json.
    std::string benchmarkResultPath;
    int targetFrame = -1;
    long long targetCall = -1;
    int width = 0;
    int height = 0;
    int cropX = 0;
    int cropY = 0;
    int cropWidth = 0;
    int cropHeight = 0;
    double ssimThreshold = 0.99;
    bool useAngle = false;
    bool usePbuffer = true;
    bool avoidAngleLlvmpipeSamplerMipmapMinFilter = false;
    bool avoidAngleLlvmpipeExplicitLodBias = false;
    bool coherentAsFlush = false;
    bool fixIterationRPSubgroupScratch = false;
    bool deriveNumSubgroups = false;
    bool iterationRPFixBarrier = false;
    int holdMs = 0;
    // Generic environment passthrough, each entry `KEY=VALUE` (an entry with no '='
    // unsets KEY). Applied last, right before libMobileGL.so is loaded, so a knob that
    // has no dedicated field above can still be forwarded from the CI script without
    // touching this struct again.
    std::vector<std::string> envOverrides;
};

struct Result {
    bool passed = false;
    int statusCode = STATUS_OK;
    std::string message;
    std::string resultPath;
    std::string actualPath;
    std::string diffPath;
    std::string matchedGoldenPath;
    double ssim = -1.0;
    long long mismatchPixels = -1;
    // Benchmark headline numbers. Left at the defaults below unless the request asked for
    // benchmark mode; benchmarkResultPath then names the JSON with the per-frame array.
    std::string benchmarkResultPath;
    long long benchmarkFrames = -1;
    int benchmarkTailFrames = 0;
    double benchmarkTotalSeconds = -1.0;
    double benchmarkMeanMs = -1.0;
    double benchmarkMedianMs = -1.0;
    double benchmarkP95Ms = -1.0;
    double benchmarkFps = -1.0;
};

Result RunTraceReplay(const Request& request);
bool WriteResultJson(const Request& request, const Result& result);

} // namespace mobilegl_trace
