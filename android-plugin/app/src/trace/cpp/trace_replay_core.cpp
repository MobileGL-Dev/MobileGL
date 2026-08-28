#include "trace_replay_core.hpp"

#include <dlfcn.h>
#include "apitrace_exit.hpp"
#include "png.h"
#include "trace_benchmark.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef MOBILEGL_APITRACE_RETRACE_MAIN
#define MOBILEGL_APITRACE_RETRACE_MAIN main
#endif

extern "C" int MOBILEGL_APITRACE_RETRACE_MAIN(int argc, char** argv);

#if defined(__GNUC__) || defined(__clang__)
extern "C" void mobilegl_trace_pump_events() __attribute__((weak));
#endif

namespace mobilegl_trace {
namespace {

struct RgbaImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;
};

class ScopedFdRedirect {
public:
    explicit ScopedFdRedirect(const std::string& path)
            : stdoutCopy(dup(STDOUT_FILENO)), stderrCopy(dup(STDERR_FILENO)) {
        int fd = open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0664);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
    }

    ~ScopedFdRedirect() {
        fflush(stdout);
        fflush(stderr);
        if (stdoutCopy >= 0) {
            dup2(stdoutCopy, STDOUT_FILENO);
            close(stdoutCopy);
        }
        if (stderrCopy >= 0) {
            dup2(stderrCopy, STDERR_FILENO);
            close(stderrCopy);
        }
    }

private:
    int stdoutCopy = -1;
    int stderrCopy = -1;
};

bool Exists(const std::string& path) {
    struct stat st {};
    return !path.empty() && stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool UseAngleForRequest(const Request& request) {
    if (request.backend != "DirectGLES") {
        return false;
    }
    if (request.useAngle) {
        return true;
    }
    const char* value = getenv("MOBILEGL_ESPRYT_USE_ANGLE");
    return value != nullptr && strcmp(value, "1") == 0;
}

bool EnsureDirectory(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    struct stat st {};
    if (stat(path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    return mkdir(path.c_str(), 0775) == 0 || errno == EEXIST;
}

std::string JsonEscape(const std::string& value) {
    std::ostringstream out;
    for (char ch : value) {
        switch (ch) {
            case '\\':
                out << "\\\\";
                break;
            case '"':
                out << "\\\"";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                out << ch;
                break;
        }
    }
    return out.str();
}

bool LoadMobileGL(const Request& request, std::string& error) {
    setenv("MOBILEGL_BACKEND_TYPE", request.backend.c_str(), 1);
    setenv("MOBILEGL_TRACE_LIBRARY", request.mobileGlLibrary.c_str(), 1);
    setenv("MOBILEGL_TRACE_SKIP_AUTODESTROY", "1", 1);
    // Retrace is a test lane on every platform, including the Android AVD one where
    // MobileGL's __ANDROID__ default would leave validation off. No overwrite: an outer
    // MOBILEGL_VALIDATE_SPIRV=0 must keep working as the escape hatch, and retracing the
    // exact shipping pipeline must stay possible.
    setenv("MOBILEGL_VALIDATE_SPIRV", "1", 0);
    setenv("MOBILEGL_TRACE_SURFACE", request.usePbuffer ? "pbuffer" : "window", 1);
    if (request.backend == "DirectVulkan") {
        setenv("MOBILEGL_MAGMA_R11G11B10F_FALLBACK", "1", 1);
    } else {
        unsetenv("MOBILEGL_MAGMA_R11G11B10F_FALLBACK");
    }
    if (UseAngleForRequest(request)) {
        setenv("MOBILEGL_ESPRYT_USE_ANGLE", "1", 1);
        setenv("MOBILEGL_TRACE_ANGLE_VARIANT", request.angleVariant.c_str(), 1);
    } else {
        unsetenv("MOBILEGL_ESPRYT_USE_ANGLE");
        unsetenv("MOBILEGL_TRACE_ANGLE_VARIANT");
    }
    if (request.avoidAngleLlvmpipeSamplerMipmapMinFilter) {
        setenv("MOBILEGL_ESPRYT_AVOID_SAMPLER_MIPMAP_MIN_FILTER", "1", 1);
    } else {
        unsetenv("MOBILEGL_ESPRYT_AVOID_SAMPLER_MIPMAP_MIN_FILTER");
    }
    if (request.avoidAngleLlvmpipeExplicitLodBias) {
        setenv("MOBILEGL_ESPRYT_AVOID_EXPLICIT_LOD_BIAS", "1", 1);
    } else {
        unsetenv("MOBILEGL_ESPRYT_AVOID_EXPLICIT_LOD_BIAS");
    }
    if (request.coherentAsFlush) {
        setenv("MOBILEGL_COHERENT_AS_FLUSH", "1", 1);
    } else {
        unsetenv("MOBILEGL_COHERENT_AS_FLUSH");
    }
    if (request.fixIterationRPSubgroupScratch) {
        setenv("MOBILEGL_MAGMA_FIX_ITERATIONRP_SUBGROUP_SCRATCH", "1", 1);
    } else {
        unsetenv("MOBILEGL_MAGMA_FIX_ITERATIONRP_SUBGROUP_SCRATCH");
    }
    if (request.deriveNumSubgroups) {
        setenv("MOBILEGL_MAGMA_DERIVE_NUM_SUBGROUPS", "1", 1);
    } else {
        unsetenv("MOBILEGL_MAGMA_DERIVE_NUM_SUBGROUPS");
    }
    if (request.iterationRPFixBarrier) {
        setenv("MOBILEGL_MAGMA_ITERATIONRP_FIX_BARRIER", "1", 1);
    } else {
        unsetenv("MOBILEGL_MAGMA_ITERATIONRP_FIX_BARRIER");
    }
    if (request.fboAttachmentDumps.empty()) {
        unsetenv("MOBILEGL_TRACE_DUMP_FBO_ATTACHMENTS");
    } else {
        std::string dumpPoints;
        for (const std::string& dumpPoint : request.fboAttachmentDumps) {
            if (!dumpPoints.empty()) {
                dumpPoints += ';';
            }
            dumpPoints += dumpPoint;
        }
        setenv("MOBILEGL_TRACE_DUMP_FBO_ATTACHMENTS", dumpPoints.c_str(), 1);
    }
    if (request.texture2dDumps.empty()) {
        unsetenv("MOBILEGL_TRACE_DUMP_TEXTURE_2D");
    } else {
        std::string dumpPoints;
        for (const std::string& dumpPoint : request.texture2dDumps) {
            if (!dumpPoints.empty()) {
                dumpPoints += ';';
            }
            dumpPoints += dumpPoint;
        }
        setenv("MOBILEGL_TRACE_DUMP_TEXTURE_2D", dumpPoints.c_str(), 1);
    }

    void* handle = dlopen(request.mobileGlLibrary.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (handle == nullptr) {
        const char* dlError = dlerror();
        error = dlError == nullptr ? "dlopen(libMobileGL.so) failed" : dlError;
        return false;
    }
    return true;
}

void ConfigureHoldEnv(const Request& request) {
    if (request.holdMs <= 0) {
        unsetenv("MOBILEGL_TRACE_HOLD_MS");
        unsetenv("MOBILEGL_TRACE_HOLD_CALL");
        unsetenv("MOBILEGL_TRACE_HOLD_DONE");
        return;
    }

    const std::string holdMs = std::to_string(request.holdMs);
    const std::string holdCall = std::to_string(request.targetCall);
    setenv("MOBILEGL_TRACE_HOLD_MS", holdMs.c_str(), 1);
    setenv("MOBILEGL_TRACE_HOLD_CALL", holdCall.c_str(), 1);
    unsetenv("MOBILEGL_TRACE_HOLD_DONE");
}

bool TraceHoldAlreadyRan() {
    const char* holdDone = getenv("MOBILEGL_TRACE_HOLD_DONE");
    return holdDone != nullptr && std::strcmp(holdDone, "1") == 0;
}

bool CopyFile(const std::string& from, const std::string& to) {
    std::ifstream input(from, std::ios::binary);
    std::ofstream output(to, std::ios::binary | std::ios::trunc);
    if (!input || !output) {
        return false;
    }
    output << input.rdbuf();
    return static_cast<bool>(output);
}

bool ReadPngRgba(const std::string& path, RgbaImage& image, std::string& error) {
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        error = "failed to open PNG: " + path;
        return false;
    }

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (png == nullptr) {
        fclose(file);
        error = "png_create_read_struct failed";
        return false;
    }

    png_infop info = png_create_info_struct(png);
    if (info == nullptr) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        fclose(file);
        error = "png_create_info_struct failed";
        return false;
    }

    if (setjmp(png_jmpbuf(png)) != 0) {
        png_destroy_read_struct(&png, &info, nullptr);
        fclose(file);
        error = "libpng failed to decode: " + path;
        return false;
    }

    png_init_io(png, file);
    png_read_info(png, info);

    png_uint_32 width = png_get_image_width(png, info);
    png_uint_32 height = png_get_image_height(png, info);
    int colorType = png_get_color_type(png, info);
    int bitDepth = png_get_bit_depth(png, info);

    if (bitDepth == 16) {
        png_set_strip_16(png);
    }
    if (colorType == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(png);
    }
    if (colorType == PNG_COLOR_TYPE_GRAY && bitDepth < 8) {
        png_set_expand_gray_1_2_4_to_8(png);
    }
    if (png_get_valid(png, info, PNG_INFO_tRNS)) {
        png_set_tRNS_to_alpha(png);
    }
    if (colorType == PNG_COLOR_TYPE_GRAY || colorType == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png);
    }
    if ((colorType & PNG_COLOR_MASK_ALPHA) == 0) {
        png_set_filler(png, 0xff, PNG_FILLER_AFTER);
    }

    png_read_update_info(png, info);
    png_size_t rowBytes = png_get_rowbytes(png, info);
    if (width == 0 || height == 0 || rowBytes < width * 4) {
        png_destroy_read_struct(&png, &info, nullptr);
        fclose(file);
        error = "decoded PNG has invalid dimensions: " + path;
        return false;
    }

    image.width = static_cast<int>(width);
    image.height = static_cast<int>(height);
    image.pixels.resize(static_cast<std::size_t>(image.width) * image.height * 4);

    std::vector<std::uint8_t> rowsStorage;
    std::vector<png_bytep> rows(height);
    if (rowBytes == width * 4) {
        for (png_uint_32 y = 0; y < height; ++y) {
            rows[y] = image.pixels.data() + static_cast<std::size_t>(y) * image.width * 4;
        }
    } else {
        rowsStorage.resize(static_cast<std::size_t>(rowBytes) * height);
        for (png_uint_32 y = 0; y < height; ++y) {
            rows[y] = rowsStorage.data() + static_cast<std::size_t>(y) * rowBytes;
        }
    }

    png_read_image(png, rows.data());
    png_read_end(png, nullptr);
    png_destroy_read_struct(&png, &info, nullptr);
    fclose(file);

    if (!rowsStorage.empty()) {
        for (int y = 0; y < image.height; ++y) {
            memcpy(image.pixels.data() + static_cast<std::size_t>(y) * image.width * 4,
                   rowsStorage.data() + static_cast<std::size_t>(y) * rowBytes,
                   static_cast<std::size_t>(image.width) * 4);
        }
    }

    return true;
}

bool WritePngRgba(const std::string& path, const RgbaImage& image, std::string& error) {
    FILE* file = fopen(path.c_str(), "wb");
    if (file == nullptr) {
        error = "failed to open PNG for write: " + path;
        return false;
    }

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (png == nullptr) {
        fclose(file);
        error = "png_create_write_struct failed";
        return false;
    }

    png_infop info = png_create_info_struct(png);
    if (info == nullptr) {
        png_destroy_write_struct(&png, nullptr);
        fclose(file);
        error = "png_create_info_struct failed";
        return false;
    }

    if (setjmp(png_jmpbuf(png)) != 0) {
        png_destroy_write_struct(&png, &info);
        fclose(file);
        error = "libpng failed to write: " + path;
        return false;
    }

    png_init_io(png, file);
    png_set_IHDR(png, info, image.width, image.height, 8, PNG_COLOR_TYPE_RGBA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    std::vector<png_bytep> rows(static_cast<std::size_t>(image.height));
    for (int y = 0; y < image.height; ++y) {
        rows[static_cast<std::size_t>(y)] =
                const_cast<png_bytep>(image.pixels.data() + static_cast<std::size_t>(y) * image.width * 4);
    }
    png_write_image(png, rows.data());
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    fclose(file);
    return true;
}

void ForceOpaqueAlpha(RgbaImage& image) {
    for (std::size_t i = 3; i < image.pixels.size(); i += 4) {
        image.pixels[i] = 0xff;
    }
}

std::string SnapshotPathForCall(const Request& request) {
    char call[16];
    snprintf(call, sizeof(call), "%010lld", request.targetCall);
    return request.outputDir + "/actual." + call + ".png";
}

// The dump hook rides on apitrace's snapshot path, which only runs for calls in the -S
// callset, so every dump point has to join the target call there.
std::string SnapshotCallSet(const Request& request) {
    std::string callSet = std::to_string(request.targetCall);
    for (const std::string& dumpPoint : request.fboAttachmentDumps) {
        const std::size_t separator = dumpPoint.find(':');
        const std::string call = dumpPoint.substr(0, separator);
        if (!call.empty() && call != std::to_string(request.targetCall)) {
            callSet += "," + call;
        }
    }
    for (const std::string& dumpPoint : request.texture2dDumps) {
        const std::size_t separator = dumpPoint.find(',');
        const std::string call = dumpPoint.substr(0, separator);
        if (!call.empty() && call != std::to_string(request.targetCall)) {
            callSet += "," + call;
        }
    }
    return callSet;
}

int RunRetraceMain(const Request& request) {
    std::vector<std::string> args;
    args.emplace_back("mobilegl-glretrace");
    args.emplace_back("-b");
    args.emplace_back("--singlethread");
    args.emplace_back("--no-context-check");
    if (!request.benchmark) {
        // The snapshot callset is also what stops the replay: apitrace exits once it has
        // dumped the last call in -S. Benchmark mode wants the whole trace, so the -s/-S
        // pair is left off entirely, which drops the readback and the PNG encode with it.
        args.emplace_back("--snapshot-alpha");
        args.emplace_back("-s");
        args.emplace_back(request.outputDir + "/actual.");
        args.emplace_back("-S");
        args.emplace_back(SnapshotCallSet(request));
    }
    args.emplace_back(request.tracePath);

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (std::string& arg : args) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    return MOBILEGL_APITRACE_RETRACE_MAIN(static_cast<int>(args.size()), argv.data());
}

bool RunRetrace(const Request& request, Result& result) {
    int status = 0;
    ConfigureHoldEnv(request);
    try {
        ScopedFdRedirect redirect(request.outputDir + "/retrace.log");
        status = RunRetraceMain(request);
    } catch (const MobileGLRetraceExit& retraceExit) {
        status = retraceExit.status;
    } catch (const std::exception& exception) {
        result.statusCode = STATUS_RETRACE_FAILED;
        result.message = "retrace failed with exception: " + std::string(exception.what());
        return false;
    } catch (...) {
        result.statusCode = STATUS_RETRACE_FAILED;
        result.message = "retrace failed with unknown exception";
        return false;
    }

    if (status != 0) {
        std::ostringstream message;
        message << "retrace failed with status " << status;
        result.statusCode = STATUS_RETRACE_FAILED;
        result.message = message.str();
        return false;
    }

    if (request.benchmark) {
        // Nothing was snapshotted, so there is nothing to collect or compare here; the
        // caller turns the recorded frame times into the result instead.
        return true;
    }

    std::string snapshotPath = SnapshotPathForCall(request);
    if (!Exists(snapshotPath)) {
        result.statusCode = STATUS_RETRACE_FAILED;
        result.message = "retrace completed but did not create expected snapshot: " + snapshotPath;
        return false;
    }

    RgbaImage snapshot;
    std::string imageError;
    if (!ReadPngRgba(snapshotPath, snapshot, imageError)) {
        result.statusCode = STATUS_IO_ERROR;
        result.message = imageError.empty()
                                 ? "failed to decode snapshot PNG: " + snapshotPath
                                 : imageError;
        return false;
    }
    ForceOpaqueAlpha(snapshot);
    if (!WritePngRgba(result.actualPath, snapshot, imageError)) {
        result.statusCode = STATUS_IO_ERROR;
        result.message = imageError.empty()
                                 ? "failed to write snapshot to actual PNG"
                                 : imageError;
        return false;
    }
    return true;
}

int ChannelValue(const RgbaImage& image, int x, int y, int channel) {
    return image.pixels[(static_cast<std::size_t>(y) * image.width + x) * 4 + channel];
}

bool WriteDifferenceImage(const Result& result,
                          const RgbaImage& actual,
                          const RgbaImage& golden,
                          int x0,
                          int y0,
                          int compareWidth,
                          int compareHeight,
                          std::string& error) {
    if (result.diffPath.empty()) {
        return true;
    }

    constexpr int kDiffScale = 8;
    RgbaImage diff;
    diff.width = actual.width;
    diff.height = actual.height;
    diff.pixels.assign(static_cast<std::size_t>(diff.width) * diff.height * 4, 0);
    for (int y = 0; y < diff.height; ++y) {
        for (int x = 0; x < diff.width; ++x) {
            std::uint8_t* dst = diff.pixels.data() + (static_cast<std::size_t>(y) * diff.width + x) * 4;
            dst[3] = 0xff;
        }
    }

    for (int y = 0; y < compareHeight; ++y) {
        for (int x = 0; x < compareWidth; ++x) {
            int imageX = x0 + x;
            int imageY = y0 + y;
            int dr = std::abs(ChannelValue(actual, imageX, imageY, 0) -
                              ChannelValue(golden, imageX, imageY, 0));
            int dg = std::abs(ChannelValue(actual, imageX, imageY, 1) -
                              ChannelValue(golden, imageX, imageY, 1));
            int db = std::abs(ChannelValue(actual, imageX, imageY, 2) -
                              ChannelValue(golden, imageX, imageY, 2));
            bool different = dr != 0 || dg != 0 || db != 0;
            std::uint8_t* dst = diff.pixels.data() +
                                (static_cast<std::size_t>(imageY) * diff.width + imageX) * 4;
            if (different) {
                dst[0] = 0xff;
                dst[1] = static_cast<std::uint8_t>(std::min(255, dg * kDiffScale));
                dst[2] = static_cast<std::uint8_t>(std::min(255, db * kDiffScale));
            } else {
                dst[0] = static_cast<std::uint8_t>(std::min(255, dr * kDiffScale));
                dst[1] = static_cast<std::uint8_t>(std::min(255, dg * kDiffScale));
                dst[2] = static_cast<std::uint8_t>(std::min(255, db * kDiffScale));
            }
        }
    }

    return WritePngRgba(result.diffPath, diff, error);
}

void PumpTraceEvents() {
#if defined(__GNUC__) || defined(__clang__)
    if (mobilegl_trace_pump_events != nullptr) {
        mobilegl_trace_pump_events();
    }
#endif
}

void HoldAfterRetrace(const Request& request) {
    if (request.holdMs <= 0 || TraceHoldAlreadyRan()) {
        return;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(request.holdMs);
    while (std::chrono::steady_clock::now() < deadline) {
        PumpTraceEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    PumpTraceEvents();
    setenv("MOBILEGL_TRACE_HOLD_DONE", "1", 1);
}

struct GoldenComparison {
    std::string path;
    RgbaImage image;
    double ssim = -1.0;
    long long mismatchPixels = 0;
    int x0 = 0;
    int y0 = 0;
    int compareWidth = 0;
    int compareHeight = 0;
};

double ComputeChannelSsim(const RgbaImage& actual,
                          const RgbaImage& golden,
                          int x0,
                          int y0,
                          int compareWidth,
                          int compareHeight,
                          unsigned channel) {
    const double count = static_cast<double>(compareWidth) * static_cast<double>(compareHeight);
    double sumA = 0.0;
    double sumG = 0.0;
    double sumAA = 0.0;
    double sumGG = 0.0;
    double sumAG = 0.0;

    for (int y = 0; y < compareHeight; ++y) {
        for (int x = 0; x < compareWidth; ++x) {
            const double a = ChannelValue(actual, x0 + x, y0 + y, channel);
            const double g = ChannelValue(golden, x0 + x, y0 + y, channel);
            sumA += a;
            sumG += g;
            sumAA += a * a;
            sumGG += g * g;
            sumAG += a * g;
        }
    }

    const double meanA = sumA / count;
    const double meanG = sumG / count;
    const double varianceA = std::max(0.0, sumAA / count - meanA * meanA);
    const double varianceG = std::max(0.0, sumGG / count - meanG * meanG);
    const double covariance = sumAG / count - meanA * meanG;

    constexpr double kC1 = 6.5025;  // (0.01 * 255)^2
    constexpr double kC2 = 58.5225; // (0.03 * 255)^2
    const double luminance = (2.0 * meanA * meanG + kC1) /
                             (meanA * meanA + meanG * meanG + kC1);
    const double contrastStructure = (2.0 * covariance + kC2) /
                                     (varianceA + varianceG + kC2);
    return luminance * contrastStructure;
}

double ComputeRgbSsim(const RgbaImage& actual,
                      const RgbaImage& golden,
                      int x0,
                      int y0,
                      int compareWidth,
                      int compareHeight) {
    double sum = 0.0;
    for (unsigned channel = 0; channel < 3; ++channel) {
        sum += ComputeChannelSsim(actual, golden, x0, y0, compareWidth, compareHeight, channel);
    }
    return sum / 3.0;
}

bool CompareAgainstOneGolden(const Request& request,
                             const RgbaImage& actual,
                             const std::string& goldenPath,
                             GoldenComparison& comparison,
                             std::string& error) {
    if (!Exists(goldenPath)) {
        error = "golden_path does not exist or is not a regular file: " + goldenPath;
        return false;
    }

    RgbaImage golden;
    if (!ReadPngRgba(goldenPath, golden, error)) {
        return false;
    }

    int x0 = request.cropX;
    int y0 = request.cropY;
    if (request.cropWidth <= 0 && request.cropHeight <= 0 &&
        (actual.width != golden.width || actual.height != golden.height)) {
        std::ostringstream message;
        message << "actual image size " << actual.width << "x" << actual.height
                << " does not match golden image size " << golden.width << "x" << golden.height
                << ": " << goldenPath;
        error = message.str();
        return false;
    }

    int compareWidth = request.cropWidth > 0 ? request.cropWidth : actual.width;
    int compareHeight = request.cropHeight > 0 ? request.cropHeight : actual.height;
    if (compareWidth <= 0 || compareHeight <= 0 ||
        x0 < 0 || y0 < 0 ||
        x0 + compareWidth > actual.width ||
        y0 + compareHeight > actual.height ||
        x0 + compareWidth > golden.width ||
        y0 + compareHeight > golden.height) {
        error = "compare crop is outside actual or golden image bounds: " + goldenPath;
        return false;
    }

    long long exactMismatch = 0;
    for (int y = 0; y < compareHeight; ++y) {
        for (int x = 0; x < compareWidth; ++x) {
            bool different = false;
            for (unsigned c = 0; c < 3; ++c) {
                int a = ChannelValue(actual, x0 + x, y0 + y, c);
                int g = ChannelValue(golden, x0 + x, y0 + y, c);
                if (a != g) {
                    different = true;
                    break;
                }
            }
            if (different) {
                ++exactMismatch;
            }
        }
    }

    comparison.path = goldenPath;
    comparison.image = std::move(golden);
    comparison.ssim = ComputeRgbSsim(actual, comparison.image, x0, y0, compareWidth, compareHeight);
    comparison.mismatchPixels = exactMismatch;
    comparison.x0 = x0;
    comparison.y0 = y0;
    comparison.compareWidth = compareWidth;
    comparison.compareHeight = compareHeight;
    return true;
}

bool CompareWithGolden(const Request& request, Result& result) {
    std::vector<std::string> goldenPaths;
    if (!request.goldenPath.empty()) {
        goldenPaths.push_back(request.goldenPath);
    }
    for (const auto& alternateGoldenPath : request.alternateGoldenPaths) {
        if (!alternateGoldenPath.empty()) {
            goldenPaths.push_back(alternateGoldenPath);
        }
    }

    if (goldenPaths.empty()) {
        result.passed = true;
        result.statusCode = STATUS_OK;
        result.message = "retrace completed; golden_path was not provided";
        result.ssim = 1.0;
        result.mismatchPixels = 0;
        return true;
    }

    RgbaImage actual;
    std::string pngError;
    if (!ReadPngRgba(result.actualPath, actual, pngError)) {
        result.statusCode = STATUS_COMPARE_FAILED;
        result.message = pngError.empty() ? "failed to decode actual PNG" : pngError;
        return false;
    }

    GoldenComparison bestComparison;
    std::string comparisonError;
    bool hasComparison = false;
    for (const auto& goldenPath : goldenPaths) {
        GoldenComparison comparison;
        std::string error;
        if (!CompareAgainstOneGolden(request, actual, goldenPath, comparison, error)) {
            comparisonError = error;
            continue;
        }
        if (!hasComparison || comparison.ssim > bestComparison.ssim) {
            bestComparison = std::move(comparison);
            hasComparison = true;
        }
    }

    if (!hasComparison) {
        result.statusCode = STATUS_COMPARE_FAILED;
        result.message = comparisonError.empty() ? "failed to compare against any golden PNG" : comparisonError;
        return false;
    }

    std::string diffError;
    if (!WriteDifferenceImage(result, actual, bestComparison.image, bestComparison.x0, bestComparison.y0,
                              bestComparison.compareWidth, bestComparison.compareHeight, diffError)) {
        result.statusCode = STATUS_IO_ERROR;
        result.message = diffError.empty() ? "failed to write diff PNG" : diffError;
        return false;
    }

    result.ssim = bestComparison.ssim;
    result.mismatchPixels = bestComparison.mismatchPixels;
    result.matchedGoldenPath = bestComparison.path;
    result.passed = bestComparison.ssim >= request.ssimThreshold;
    result.statusCode = result.passed ? STATUS_OK : STATUS_COMPARE_FAILED;
    std::ostringstream message;
    message << std::fixed << std::setprecision(6)
            << "retrace completed; ssim=" << bestComparison.ssim
            << ", ssimThreshold=" << request.ssimThreshold
            << ", mismatchPixels=" << bestComparison.mismatchPixels
            << ", matchedGoldenPath=" << bestComparison.path;
    result.message = message.str();
    return result.passed;
}

std::string BenchmarkResultPath(const Request& request) {
    return request.benchmarkResultPath.empty() ? request.outputDir + "/benchmark.json"
                                               : request.benchmarkResultPath;
}

// Folds the recorded frame times into the headline numbers. Everything but totalSeconds and
// the frame count is computed over the trailing benchmarkTailFrames frames only.
void SummarizeBenchmark(const Request& request, const benchmark::Report& report, Result& result) {
    result.benchmarkFrames = static_cast<long long>(report.frameMs.size());
    result.benchmarkTotalSeconds = report.totalSeconds;
    result.benchmarkTailFrames = 0;
    if (report.frameMs.empty()) {
        return;
    }

    const int requestedTail =
            request.benchmarkTailFrames > 0 ? request.benchmarkTailFrames : kDefaultBenchmarkTailFrames;
    const std::size_t tail =
            std::min(static_cast<std::size_t>(requestedTail), report.frameMs.size());
    result.benchmarkTailFrames = static_cast<int>(tail);

    std::vector<double> window(report.frameMs.end() - static_cast<std::ptrdiff_t>(tail),
                               report.frameMs.end());
    double sum = 0.0;
    for (double frameMs : window) {
        sum += frameMs;
    }
    result.benchmarkMeanMs = sum / static_cast<double>(tail);

    std::sort(window.begin(), window.end());
    result.benchmarkMedianMs = (tail % 2 == 1)
                                       ? window[tail / 2]
                                       : 0.5 * (window[tail / 2 - 1] + window[tail / 2]);
    // Nearest-rank p95, so the reported value is always an observed frame time.
    std::size_t rank = static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(tail)));
    if (rank == 0) {
        rank = 1;
    }
    result.benchmarkP95Ms = window[rank - 1];
    result.benchmarkFps = result.benchmarkMeanMs > 0.0 ? 1000.0 / result.benchmarkMeanMs : -1.0;
}

bool WriteBenchmarkJson(const Request& request,
                        const Result& result,
                        const benchmark::Report& report) {
    std::ofstream file(result.benchmarkResultPath, std::ios::out | std::ios::trunc);
    if (!file) {
        return false;
    }
    file << "{\n";
    file << "  \"tracePath\": \"" << JsonEscape(request.tracePath) << "\",\n";
    file << "  \"backend\": \"" << JsonEscape(request.backend) << "\",\n";
    file << "  \"benchmarkFinish\": " << (request.benchmarkFinish ? "true" : "false") << ",\n";
    file << "  \"width\": " << request.width << ",\n";
    file << "  \"height\": " << request.height << ",\n";
    file << "  \"totalFrames\": " << result.benchmarkFrames << ",\n";
    file << "  \"tailFrames\": " << result.benchmarkTailFrames << ",\n";
    file << std::fixed << std::setprecision(6);
    file << "  \"totalSeconds\": " << result.benchmarkTotalSeconds << ",\n";
    file << std::setprecision(3);
    file << "  \"meanFrameMs\": " << result.benchmarkMeanMs << ",\n";
    file << "  \"medianFrameMs\": " << result.benchmarkMedianMs << ",\n";
    file << "  \"p95FrameMs\": " << result.benchmarkP95Ms << ",\n";
    file << "  \"fps\": " << result.benchmarkFps << ",\n";
    file << "  \"frameTimesMs\": [";
    for (std::size_t i = 0; i < report.frameMs.size(); ++i) {
        if (i > 0) {
            file << ", ";
        }
        file << report.frameMs[i];
    }
    file << "]\n";
    file << "}\n";
    return static_cast<bool>(file);
}

} // namespace

extern "C" [[noreturn]] void mobilegl_apitrace_exit(int status) {
    throw MobileGLRetraceExit{status};
}

bool WriteResultJson(const Request& request, const Result& result) {
    std::ofstream file(result.resultPath, std::ios::out | std::ios::trunc);
    if (!file) {
        return false;
    }
    file << "{\n";
    file << "  \"passed\": " << (result.passed ? "true" : "false") << ",\n";
    file << "  \"statusCode\": " << result.statusCode << ",\n";
    file << "  \"message\": \"" << JsonEscape(result.message) << "\",\n";
    file << "  \"tracePath\": \"" << JsonEscape(request.tracePath) << "\",\n";
    file << "  \"goldenPath\": \"" << JsonEscape(request.goldenPath) << "\",\n";
    file << "  \"alternateGoldenPaths\": [";
    for (std::size_t i = 0; i < request.alternateGoldenPaths.size(); ++i) {
        if (i > 0) {
            file << ", ";
        }
        file << "\"" << JsonEscape(request.alternateGoldenPaths[i]) << "\"";
    }
    file << "],\n";
    file << "  \"matchedGoldenPath\": \"" << JsonEscape(result.matchedGoldenPath) << "\",\n";
    file << "  \"actualPath\": \"" << JsonEscape(result.actualPath) << "\",\n";
    file << "  \"diffPath\": \"" << JsonEscape(result.diffPath) << "\",\n";
    file << "  \"backend\": \"" << JsonEscape(request.backend) << "\",\n";
    file << "  \"angleVariant\": \"" << JsonEscape(request.angleVariant) << "\",\n";
    file << "  \"targetFrame\": " << request.targetFrame << ",\n";
    file << "  \"targetCall\": " << request.targetCall << ",\n";
    file << "  \"width\": " << request.width << ",\n";
    file << "  \"height\": " << request.height << ",\n";
    file << "  \"cropX\": " << request.cropX << ",\n";
    file << "  \"cropY\": " << request.cropY << ",\n";
    file << "  \"cropWidth\": " << request.cropWidth << ",\n";
    file << "  \"cropHeight\": " << request.cropHeight << ",\n";
    file << std::fixed << std::setprecision(9);
    file << "  \"ssim\": " << result.ssim << ",\n";
    file << "  \"ssimThreshold\": " << request.ssimThreshold << ",\n";
    file << "  \"useAngle\": " << (UseAngleForRequest(request) ? "true" : "false") << ",\n";
    file << "  \"usePbuffer\": " << (request.usePbuffer ? "true" : "false") << ",\n";
    file << "  \"avoidAngleLlvmpipeSamplerMipmapMinFilter\": "
         << (request.avoidAngleLlvmpipeSamplerMipmapMinFilter ? "true" : "false") << ",\n";
    file << "  \"avoidAngleLlvmpipeExplicitLodBias\": "
         << (request.avoidAngleLlvmpipeExplicitLodBias ? "true" : "false") << ",\n";
    file << "  \"fixIterationRPSubgroupScratch\": " << (request.fixIterationRPSubgroupScratch ? "true" : "false")
         << ",\n";
    file << "  \"deriveNumSubgroups\": " << (request.deriveNumSubgroups ? "true" : "false") << ",\n";
    file << "  \"iterationRPFixBarrier\": " << (request.iterationRPFixBarrier ? "true" : "false") << ",\n";
    file << "  \"holdMs\": " << request.holdMs << ",\n";
    file << "  \"mismatchPixels\": " << result.mismatchPixels;
    if (request.benchmark) {
        // Headline numbers only; the per-frame array lives in benchmarkResultPath.
        file << ",\n";
        file << "  \"benchmark\": true,\n";
        file << "  \"benchmarkResultPath\": \"" << JsonEscape(result.benchmarkResultPath) << "\",\n";
        file << "  \"benchmarkFinish\": " << (request.benchmarkFinish ? "true" : "false") << ",\n";
        file << "  \"benchmarkFrames\": " << result.benchmarkFrames << ",\n";
        file << "  \"benchmarkTailFrames\": " << result.benchmarkTailFrames << ",\n";
        file << std::setprecision(6);
        file << "  \"benchmarkTotalSeconds\": " << result.benchmarkTotalSeconds << ",\n";
        file << std::setprecision(3);
        file << "  \"benchmarkMeanFrameMs\": " << result.benchmarkMeanMs << ",\n";
        file << "  \"benchmarkMedianFrameMs\": " << result.benchmarkMedianMs << ",\n";
        file << "  \"benchmarkP95FrameMs\": " << result.benchmarkP95Ms << ",\n";
        file << "  \"benchmarkFps\": " << result.benchmarkFps << "\n";
    } else {
        file << "\n";
    }
    file << "}\n";
    return true;
}

Result RunTraceReplay(const Request& request) {
    Result result;
    result.resultPath = request.outputDir + "/result.json";
    result.actualPath = request.outputDir + "/actual.png";
    result.diffPath = request.diffPath;
    if (request.benchmark) {
        result.benchmarkResultPath = BenchmarkResultPath(request);
    }
    const std::string mobileGlLogPath = request.outputDir + "/mobilegl.log";

    if (!EnsureDirectory(request.outputDir)) {
        result.statusCode = STATUS_IO_ERROR;
        result.message = "failed to create output directory: " + request.outputDir;
        return result;
    }

    if (request.backend != "DirectGLES" && request.backend != "DirectVulkan") {
        result.statusCode = STATUS_INVALID_ARGUMENT;
        result.message = "backend must be DirectGLES or DirectVulkan";
        return result;
    }

    if (!Exists(request.tracePath)) {
        result.statusCode = STATUS_INVALID_ARGUMENT;
        result.message = "trace_path does not exist or is not a regular file";
        return result;
    }

    // Benchmark mode never snapshots, so it has no target call to stop at.
    if (!request.benchmark && request.targetCall < 0) {
        result.statusCode = STATUS_INVALID_ARGUMENT;
        result.message = "target_call must be set for dump-images style replay";
        return result;
    }

    setenv("MOBILEGL_LOG_FILE_PATH", mobileGlLogPath.c_str(), 1);

    std::string mobileGlError;
    if (!LoadMobileGL(request, mobileGlError)) {
        result.statusCode = STATUS_MOBILEGL_LOAD_ERROR;
        result.message = "failed to load MobileGL: " + mobileGlError;
        return result;
    }

    if (request.benchmark) {
        benchmark::Begin(request.benchmarkFinish);
        const bool retraced = RunRetrace(request, result);
        const benchmark::Report report = benchmark::End();
        SummarizeBenchmark(request, report, result);
        // Written even when the retrace failed: a partial timing series says where the
        // replay got to, which is exactly what is wanted when triaging one.
        const bool wroteJson = WriteBenchmarkJson(request, result, report);
        HoldAfterRetrace(request);
        if (!retraced) {
            return result;
        }
        if (!wroteJson) {
            result.statusCode = STATUS_IO_ERROR;
            result.message = "benchmark completed but failed to write " + result.benchmarkResultPath;
            return result;
        }
        // "Passed" in benchmark mode means the replay ran the trace to the end without
        // error; there is no golden to be right or wrong about.
        result.passed = true;
        result.statusCode = STATUS_OK;
        std::ostringstream message;
        message << std::fixed << std::setprecision(3)
                << "benchmark completed; frames=" << result.benchmarkFrames
                << ", tailFrames=" << result.benchmarkTailFrames
                << ", meanMs=" << result.benchmarkMeanMs
                << ", medianMs=" << result.benchmarkMedianMs
                << ", p95Ms=" << result.benchmarkP95Ms
                << ", fps=" << result.benchmarkFps
                << ", benchmarkResultPath=" << result.benchmarkResultPath;
        result.message = message.str();
        return result;
    }

    if (!RunRetrace(request, result)) {
        HoldAfterRetrace(request);
        return result;
    }

    HoldAfterRetrace(request);
    CompareWithGolden(request, result);
    return result;
}

} // namespace mobilegl_trace
