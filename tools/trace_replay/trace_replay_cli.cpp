#include "trace_replay_core.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

extern "C" void mobilegl_trace_set_requested_size(int width, int height);

namespace {

void PrintUsage(const char *argv0) {
    std::cerr
            << "Usage: " << argv0 << " --trace PATH --target-call N --output DIR [options]\n"
            << "\n"
            << "Options:\n"
            << "  --golden PATH             Golden PNG to compare against\n"
            << "  --alternate-golden PATH   Additional acceptable golden PNG\n"
            << "  --diff PATH               Difference PNG output path\n"
            << "  --backend NAME            DirectGLES or DirectVulkan (default: DirectGLES)\n"
            << "  --mobilegl-library PATH   libMobileGL.so path (default: libMobileGL.so)\n"
            << "  --width N                 Replay surface width override\n"
            << "  --height N                Replay surface height override\n"
            << "  --window-surface          Replay to a native window surface\n"
            << "  --pbuffer-surface         Replay to an EGL pbuffer surface (default)\n"
            << "  --hold-ms N               Keep the replay process alive after retrace (default: 0)\n"
            << "  --ssim-threshold N        Minimum SSIM required to pass (default: 0.99)\n"
            << "  --crop-x N                Compare crop x\n"
            << "  --crop-y N                Compare crop y\n"
            << "  --crop-width N            Compare crop width\n"
            << "  --crop-height N           Compare crop height\n"
            << "  --coherent-as-flush       Set MOBILEGL_COHERENT_AS_FLUSH=1 for the replay\n"
            << "  --benchmark               Frame-timing mode: replay the whole trace, record a\n"
            << "                            wall-clock timestamp at every frame boundary, and skip\n"
            << "                            the snapshot and the golden comparison. --target-call is\n"
            << "                            not required in this mode.\n"
            << "  --benchmark-tail-frames=N Frames at the end of the run the mean/median/p95 are\n"
            << "                            computed over (default: 200, clamped to the frame count)\n"
            << "  --benchmark-finish=0/1    glFinish at every frame boundary so a frame time covers\n"
            << "                            GPU completion and not just CPU submission (default: 1)\n"
            << "  --benchmark-result=PATH   Timing JSON output (default: OUTPUT/benchmark.json)\n"
            << "  --dump-fbo-attachments CALL:DIR[:FBO,FBO,...]\n"
            << "                            At CALL, write every colour attachment and the depth\n"
            << "                            attachment of every live framebuffer object into DIR as\n"
            << "                            fbo<N>-att<M>.png / fbo<N>-depth.png, plus a manifest.txt\n"
            << "                            of formats and per-channel statistics. Repeatable.\n";
}

bool ReadValue(int argc, char **argv, int &index, std::string &out) {
    if (index + 1 >= argc) {
        std::cerr << "Missing value for " << argv[index] << "\n";
        return false;
    }
    out = argv[++index];
    return true;
}

bool ReadInt(int argc, char **argv, int &index, int &out) {
    std::string value;
    if (!ReadValue(argc, argv, index, value)) {
        return false;
    }
    out = std::atoi(value.c_str());
    return true;
}

bool ReadLongLong(int argc, char **argv, int &index, long long &out) {
    std::string value;
    if (!ReadValue(argc, argv, index, value)) {
        return false;
    }
    out = std::strtoll(value.c_str(), nullptr, 10);
    return true;
}

bool ReadDouble(int argc, char **argv, int &index, double &out) {
    std::string value;
    if (!ReadValue(argc, argv, index, value)) {
        return false;
    }
    out = std::strtod(value.c_str(), nullptr);
    return true;
}

bool ReadEnvFlag(const char *name) {
    const char *value = std::getenv(name);
    return value != nullptr && std::string(value) == "1";
}

// The benchmark options are documented as --name=VALUE. The space-separated spelling every
// other option here uses is accepted too, so --benchmark-tail-frames=300 and
// --benchmark-tail-frames 300 both work; hasInlineValue says which one was given.
bool MatchOption(const std::string &arg, const char *name, std::string &inlineValue, bool &hasInlineValue) {
    if (arg == name) {
        hasInlineValue = false;
        return true;
    }
    const std::string prefix = std::string(name) + "=";
    if (arg.compare(0, prefix.size(), prefix) == 0) {
        inlineValue = arg.substr(prefix.size());
        hasInlineValue = true;
        return true;
    }
    return false;
}

bool ParseArgs(int argc, char **argv, mobilegl_trace::Request &request) {
    request.backend = "DirectGLES";
    request.fixIterationRPSubgroupScratch = ReadEnvFlag("MOBILEGL_FIX_ITERATIONRP_SUBGROUP_SCRATCH");
    request.deriveNumSubgroups = ReadEnvFlag("MOBILEGL_DERIVE_NUM_SUBGROUPS");
    request.iterationRPFixBarrier = ReadEnvFlag("MOBILEGL_ITERATIONRP_FIX_BARRIER");

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        std::string optionValue;
        bool optionHasValue = false;
        if (arg == "--trace") {
            if (!ReadValue(argc, argv, i, request.tracePath)) return false;
        } else if (arg == "--golden") {
            if (!ReadValue(argc, argv, i, request.goldenPath)) return false;
        } else if (arg == "--alternate-golden") {
            std::string alternateGolden;
            if (!ReadValue(argc, argv, i, alternateGolden)) return false;
            if (!alternateGolden.empty()) {
                request.alternateGoldenPaths.push_back(alternateGolden);
            }
        } else if (arg == "--diff") {
            if (!ReadValue(argc, argv, i, request.diffPath)) return false;
        } else if (arg == "--output") {
            if (!ReadValue(argc, argv, i, request.outputDir)) return false;
        } else if (arg == "--backend") {
            if (!ReadValue(argc, argv, i, request.backend)) return false;
        } else if (arg == "--mobilegl-library") {
            if (!ReadValue(argc, argv, i, request.mobileGlLibrary)) return false;
        } else if (arg == "--target-frame") {
            if (!ReadInt(argc, argv, i, request.targetFrame)) return false;
        } else if (arg == "--target-call") {
            if (!ReadLongLong(argc, argv, i, request.targetCall)) return false;
        } else if (arg == "--width") {
            if (!ReadInt(argc, argv, i, request.width)) return false;
        } else if (arg == "--height") {
            if (!ReadInt(argc, argv, i, request.height)) return false;
        } else if (arg == "--window-surface") {
            request.usePbuffer = false;
        } else if (arg == "--pbuffer-surface") {
            request.usePbuffer = true;
        } else if (arg == "--hold-ms") {
            if (!ReadInt(argc, argv, i, request.holdMs)) return false;
        } else if (arg == "--ssim-threshold") {
            if (!ReadDouble(argc, argv, i, request.ssimThreshold)) return false;
        } else if (arg == "--crop-x") {
            if (!ReadInt(argc, argv, i, request.cropX)) return false;
        } else if (arg == "--crop-y") {
            if (!ReadInt(argc, argv, i, request.cropY)) return false;
        } else if (arg == "--crop-width") {
            if (!ReadInt(argc, argv, i, request.cropWidth)) return false;
        } else if (arg == "--crop-height") {
            if (!ReadInt(argc, argv, i, request.cropHeight)) return false;
        } else if (arg == "--coherent-as-flush") {
            request.coherentAsFlush = true;
        } else if (arg == "--benchmark") {
            request.benchmark = true;
        } else if (MatchOption(arg, "--benchmark-tail-frames", optionValue, optionHasValue)) {
            if (!optionHasValue && !ReadValue(argc, argv, i, optionValue)) return false;
            request.benchmarkTailFrames = std::atoi(optionValue.c_str());
        } else if (MatchOption(arg, "--benchmark-finish", optionValue, optionHasValue)) {
            if (!optionHasValue && !ReadValue(argc, argv, i, optionValue)) return false;
            request.benchmarkFinish = optionValue != "0";
        } else if (MatchOption(arg, "--benchmark-result", optionValue, optionHasValue)) {
            if (!optionHasValue && !ReadValue(argc, argv, i, optionValue)) return false;
            request.benchmarkResultPath = optionValue;
        } else if (arg == "--dump-fbo-attachments") {
            std::string dumpPoint;
            if (!ReadValue(argc, argv, i, dumpPoint)) return false;
            if (dumpPoint.find(':') == std::string::npos) {
                std::cerr << "--dump-fbo-attachments expects CALL:DIR[:FBO,FBO,...]\n";
                return false;
            }
            request.fboAttachmentDumps.push_back(dumpPoint);
        } else if (arg == "--help" || arg == "-h") {
            return false;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return false;
        }
    }

    if (request.tracePath.empty()) {
        std::cerr << "--trace is required\n";
        return false;
    }
    if (request.outputDir.empty()) {
        std::cerr << "--output is required\n";
        return false;
    }
    if (request.targetCall < 0 && !request.benchmark) {
        std::cerr << "--target-call is required\n";
        return false;
    }
    if (request.holdMs < 0) {
        std::cerr << "--hold-ms must be non-negative\n";
        return false;
    }
    if (request.benchmarkTailFrames <= 0) {
        std::cerr << "--benchmark-tail-frames must be positive\n";
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char **argv) {
    mobilegl_trace::Request request;
    if (!ParseArgs(argc, argv, request)) {
        PrintUsage(argv[0]);
        return 2;
    }

    mobilegl_trace_set_requested_size(request.width, request.height);
    mobilegl_trace::Result result = mobilegl_trace::RunTraceReplay(request);
    mobilegl_trace_set_requested_size(0, 0);

    if (!result.resultPath.empty()) {
        mobilegl_trace::WriteResultJson(request, result);
    }

    if (request.benchmark) {
        std::cout << result.message << "\n"
                  << "result: " << result.resultPath << "\n"
                  << "benchmark: " << result.benchmarkResultPath << "\n";
    } else {
        std::cout << result.message << "\n"
                  << "result: " << result.resultPath << "\n"
                  << "actual: " << result.actualPath << "\n"
                  << "diff: " << result.diffPath << "\n";
    }
    return result.passed ? 0 : result.statusCode;
}
