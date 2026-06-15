#include "trace_replay_core.hpp"

#include <dlfcn.h>
#include "apitrace_exit.hpp"
#include "png.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" int main(int argc, char** argv);

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

    void* handle = dlopen(request.mobileGlLibrary.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (handle == nullptr) {
        const char* dlError = dlerror();
        error = dlError == nullptr ? "dlopen(libMobileGL.so) failed" : dlError;
        return false;
    }
    return true;
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

bool ReadPpmRgbAsRgba(const std::string& path, RgbaImage& image, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "failed to open PPM: " + path;
        return false;
    }

    auto readToken = [&file]() -> std::string {
        std::string token;
        char ch = 0;
        while (file.get(ch)) {
            if (ch == '#') {
                file.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                continue;
            }
            if (!std::isspace(static_cast<unsigned char>(ch))) {
                token.push_back(ch);
                break;
            }
        }
        while (file.get(ch)) {
            if (std::isspace(static_cast<unsigned char>(ch))) {
                break;
            }
            token.push_back(ch);
        }
        return token;
    };

    const std::string magic = readToken();
    const std::string widthToken = readToken();
    const std::string heightToken = readToken();
    const std::string maxToken = readToken();
    if (magic != "P6" || widthToken.empty() || heightToken.empty() || maxToken != "255") {
        error = "unsupported PPM header: " + path;
        return false;
    }

    image.width = std::stoi(widthToken);
    image.height = std::stoi(heightToken);
    if (image.width <= 0 || image.height <= 0) {
        error = "PPM has invalid dimensions: " + path;
        return false;
    }

    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(image.width) * image.height * 3);
    file.read(reinterpret_cast<char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
    if (file.gcount() != static_cast<std::streamsize>(rgb.size())) {
        error = "PPM payload is truncated: " + path;
        return false;
    }

    image.pixels.resize(static_cast<std::size_t>(image.width) * image.height * 4);
    for (std::size_t i = 0, j = 0; i < rgb.size(); i += 3, j += 4) {
        image.pixels[j + 0] = rgb[i + 0];
        image.pixels[j + 1] = rgb[i + 1];
        image.pixels[j + 2] = rgb[i + 2];
        image.pixels[j + 3] = 0xff;
    }
    return true;
}

std::string SnapshotPathForCall(const Request& request) {
    char call[16];
    snprintf(call, sizeof(call), "%010lld", request.targetCall);
    return request.outputDir + "/actual." + call + ".png";
}

int RunRetraceMain(const Request& request) {
    std::string prefix = request.outputDir + "/actual.";
    std::string callSet = std::to_string(request.targetCall);

    std::string arg0 = "mobilegl-glretrace";
    std::string argBenchmark = "-b";
    std::string argSingleThread = "--singlethread";
    std::string argNoContextCheck = "--no-context-check";
    std::string argSnapshotAlpha = "--snapshot-alpha";
    std::string argSnapshotPrefix = "-s";
    std::string argSnapshotCall = "-S";
    std::string tracePath = request.tracePath;

    char* argv[] = {
            arg0.data(),
            argBenchmark.data(),
            argSingleThread.data(),
            argNoContextCheck.data(),
            argSnapshotAlpha.data(),
            argSnapshotPrefix.data(),
            prefix.data(),
            argSnapshotCall.data(),
            callSet.data(),
            tracePath.data(),
            nullptr,
    };
    return main(10, argv);
}

bool RunRetrace(const Request& request, Result& result) {
    int status = 0;
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

    std::string snapshotPath = SnapshotPathForCall(request);
    const std::string presentPath = request.outputDir + "/present.ppm";
    const bool hasSnapshot = Exists(snapshotPath);
    const bool hasPresentDump = request.backend == "DirectVulkan" && Exists(presentPath);
    if (!hasSnapshot && !hasPresentDump) {
        result.statusCode = STATUS_RETRACE_FAILED;
        result.message = "retrace completed but did not create expected snapshot: " + snapshotPath;
        return false;
    }
    if (hasSnapshot && !hasPresentDump) {
        if (!CopyFile(snapshotPath, result.actualPath)) {
            result.statusCode = STATUS_IO_ERROR;
            result.message = "failed to copy snapshot to " + result.actualPath;
            return false;
        }
    }
    if (request.backend == "DirectVulkan") {
        if (hasPresentDump) {
            RgbaImage present;
            std::string imageError;
            if (!ReadPpmRgbAsRgba(presentPath, present, imageError) ||
                !WritePngRgba(request.outputDir + "/present.png", present, imageError)) {
                result.statusCode = STATUS_IO_ERROR;
                result.message = imageError.empty()
                                         ? "failed to convert DirectVulkan present dump to PNG"
                                         : imageError;
                return false;
            }
            if (!WritePngRgba(result.actualPath, present, imageError)) {
                result.statusCode = STATUS_IO_ERROR;
                result.message = imageError.empty()
                                         ? "failed to write DirectVulkan present dump to actual PNG"
                                         : imageError;
                return false;
            }
        }
    }
    return true;
}

int ChannelValue(const RgbaImage& image, int x, int y, int channel) {
    return image.pixels[(static_cast<std::size_t>(y) * image.width + x) * 4 + channel];
}

bool CompareWithGolden(const Request& request, Result& result) {
    if (request.goldenPath.empty()) {
        result.passed = true;
        result.statusCode = STATUS_OK;
        result.message = "retrace completed; golden_path was not provided";
        result.mismatchPixels = 0;
        return true;
    }
    if (!Exists(request.goldenPath)) {
        result.statusCode = STATUS_INVALID_ARGUMENT;
        result.message = "golden_path does not exist or is not a regular file";
        return false;
    }

    RgbaImage actual;
    RgbaImage golden;
    std::string pngError;
    if (!ReadPngRgba(result.actualPath, actual, pngError) ||
        !ReadPngRgba(request.goldenPath, golden, pngError)) {
        result.statusCode = STATUS_COMPARE_FAILED;
        result.message = pngError.empty() ? "failed to decode actual or golden PNG" : pngError;
        return false;
    }

    int x0 = request.cropX;
    int y0 = request.cropY;
    if (request.cropWidth <= 0 && request.cropHeight <= 0 &&
        (actual.width != golden.width || actual.height != golden.height)) {
        result.statusCode = STATUS_COMPARE_FAILED;
        std::ostringstream message;
        message << "actual image size " << actual.width << "x" << actual.height
                << " does not match golden image size " << golden.width << "x" << golden.height;
        result.message = message.str();
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
        result.statusCode = STATUS_INVALID_ARGUMENT;
        result.message = "compare crop is outside actual or golden image bounds";
        return false;
    }

    const int fuzz = std::max(0, std::min(100, request.fuzzPercent)) * 255 / 100;
    long long mismatch = 0;
    for (int y = 0; y < compareHeight; ++y) {
        for (int x = 0; x < compareWidth; ++x) {
            bool different = false;
            for (unsigned c = 0; c < 3; ++c) {
                int a = ChannelValue(actual, x0 + x, y0 + y, c);
                int g = ChannelValue(golden, x0 + x, y0 + y, c);
                if (std::abs(a - g) > fuzz) {
                    different = true;
                    break;
                }
            }
            if (different) {
                ++mismatch;
            }
        }
    }

    result.mismatchPixels = mismatch;
    result.passed = mismatch <= request.tolerance;
    result.statusCode = result.passed ? STATUS_OK : STATUS_COMPARE_FAILED;
    std::ostringstream message;
    message << "retrace completed; mismatchPixels=" << mismatch
            << ", tolerance=" << request.tolerance
            << ", fuzzPercent=" << request.fuzzPercent;
    result.message = message.str();
    return result.passed;
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
    file << "  \"actualPath\": \"" << JsonEscape(result.actualPath) << "\",\n";
    file << "  \"backend\": \"" << JsonEscape(request.backend) << "\",\n";
    file << "  \"targetFrame\": " << request.targetFrame << ",\n";
    file << "  \"targetCall\": " << request.targetCall << ",\n";
    file << "  \"width\": " << request.width << ",\n";
    file << "  \"height\": " << request.height << ",\n";
    file << "  \"cropX\": " << request.cropX << ",\n";
    file << "  \"cropY\": " << request.cropY << ",\n";
    file << "  \"cropWidth\": " << request.cropWidth << ",\n";
    file << "  \"cropHeight\": " << request.cropHeight << ",\n";
    file << "  \"tolerance\": " << request.tolerance << ",\n";
    file << "  \"fuzzPercent\": " << request.fuzzPercent << ",\n";
    file << "  \"mismatchPixels\": " << result.mismatchPixels << "\n";
    file << "}\n";
    return true;
}

Result RunTraceReplay(const Request& request) {
    Result result;
    result.resultPath = request.outputDir + "/result.json";
    result.actualPath = request.outputDir + "/actual.png";

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

    if (request.targetCall < 0) {
        result.statusCode = STATUS_INVALID_ARGUMENT;
        result.message = "target_call must be set for dump-images style replay";
        return result;
    }

    if (request.backend == "DirectVulkan") {
        std::string presentDumpPath = request.outputDir + "/present.ppm";
        std::string presentDumpCall = std::to_string(request.targetCall);
        setenv("MOBILEGL_PRESENT_DUMP_PATH", presentDumpPath.c_str(), 1);
        setenv("MOBILEGL_PRESENT_DUMP_CALL", presentDumpCall.c_str(), 1);
    } else {
        unsetenv("MOBILEGL_PRESENT_STATS");
        unsetenv("MOBILEGL_PRESENT_DUMP_PATH");
        unsetenv("MOBILEGL_PRESENT_DUMP_CALL");
        unsetenv("MOBILEGL_TEXTURE_UPLOAD_STATS");
        unsetenv("MOBILEGL_DESCRIPTOR_STATS");
    }

    std::string mobileGlError;
    if (!LoadMobileGL(request, mobileGlError)) {
        result.statusCode = STATUS_MOBILEGL_LOAD_ERROR;
        result.message = "failed to load MobileGL: " + mobileGlError;
        return result;
    }

    if (!RunRetrace(request, result)) {
        return result;
    }

    CompareWithGolden(request, result);
    return result;
}

} // namespace mobilegl_trace
