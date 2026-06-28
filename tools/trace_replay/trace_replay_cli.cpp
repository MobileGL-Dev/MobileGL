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
            << "  --crop-height N           Compare crop height\n";
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

bool ParseArgs(int argc, char **argv, mobilegl_trace::Request &request) {
    request.backend = "DirectGLES";

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
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
    if (request.targetCall < 0) {
        std::cerr << "--target-call is required\n";
        return false;
    }
    if (request.holdMs < 0) {
        std::cerr << "--hold-ms must be non-negative\n";
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

    std::cout << result.message << "\n"
              << "result: " << result.resultPath << "\n"
              << "actual: " << result.actualPath << "\n"
              << "diff: " << result.diffPath << "\n";
    return result.passed ? 0 : result.statusCode;
}
