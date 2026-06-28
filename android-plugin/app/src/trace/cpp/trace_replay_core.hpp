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

struct Request {
    std::string tracePath;
    std::string goldenPath;
    std::vector<std::string> alternateGoldenPaths;
    std::string outputDir;
    std::string diffPath;
    std::string backend;
    std::string mobileGlLibrary = "libMobileGL.so";
    std::string angleLibraryDir;
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
    int holdMs = 0;
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
};

Result RunTraceReplay(const Request& request);
bool WriteResultJson(const Request& request, const Result& result);

} // namespace mobilegl_trace
