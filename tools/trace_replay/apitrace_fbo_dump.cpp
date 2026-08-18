#include "apitrace_fbo_dump.hpp"

#include "glproc.hpp"
#include "image.hpp"
#include "retrace.hpp"
#include "state_writer.hpp"

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif
#include <vector>

// Dumps every colour attachment (and the depth attachment) of every live framebuffer
// object at a chosen call boundary, on both sides of a driver comparison. The intent is
// to name the first attachment whose contents diverge between two stacks; the manifest is
// formatted so that `diff` over two dump directories points straight at it.
//
// The hook rides on apitrace's snapshot path: retrace_main's takeSnapshot() asks
// retrace::dumper for its snapshot count at exactly the call boundary we want, so wrapping
// retrace::dumper gives a per-call hook without patching apitrace. trace_replay_core adds
// the dump calls to the -S callset so the hook is reached.

namespace mobilegl_trace_dump {
namespace {

using PfnGetIntegerv = void (*)(GLenum, GLint *);
using PfnGetError = GLenum (*)(void);

constexpr const char *kDumpPointsEnv = "MOBILEGL_TRACE_DUMP_FBO_ATTACHMENTS";
constexpr const char *kTexture2dDumpPointsEnv = "MOBILEGL_TRACE_DUMP_TEXTURE_2D";
constexpr const char *kScanLimitEnv = "MOBILEGL_TRACE_DUMP_FBO_SCAN_LIMIT";
constexpr unsigned kDefaultScanLimit = 1024;

struct DumpPoint {
    unsigned call = 0;
    std::string directory;
    // Empty means "every framebuffer object the driver still knows about".
    std::vector<unsigned> framebuffers;
    bool done = false;
};

// CALL,TEXTURE,LEVEL,DIR entries, separated by ';'. This deliberately does not share the
// framebuffer dump grammar: an absolute Windows directory contains ':' but not ','.
struct Texture2dDumpPoint {
    unsigned call = 0;
    unsigned texture = 0;
    unsigned level = 0;
    std::string directory;
    bool done = false;
};

struct AttachmentDesc {
    GLint objectType = GL_NONE;
    GLint objectName = 0;
    GLint level = 0;
    GLint width = 0;
    GLint height = 0;
    GLint internalFormat = 0;
    GLint componentType = GL_NONE;
};

std::vector<DumpPoint> gDumpPoints;
std::vector<Texture2dDumpPoint> gTexture2dDumpPoints;
bool gInstalled = false;
bool gConfigured = false;
retrace::Dumper *gInnerDumper = nullptr;
PfnGetIntegerv gGetIntegerv = nullptr;
PfnGetError gGetError = nullptr;

// apitrace's public dispatch is interposed by apitrace_glproc_mobilegl.cpp, which pins
// glGetIntegerv(GL_READ_BUFFER) to GL_BACK and swallows glGetError. Reading real state -
// notably each framebuffer's read buffer, which has to be restored - needs the
// uninterposed entry points.
void ResolveDirectEntryPoints() {
    if (gGetIntegerv == nullptr) {
        gGetIntegerv = reinterpret_cast<PfnGetIntegerv>(_getPrivateProcAddress("glGetIntegerv"));
    }
    if (gGetError == nullptr) {
        gGetError = reinterpret_cast<PfnGetError>(_getPrivateProcAddress("glGetError"));
    }
}

GLint GetInteger(GLenum pname) {
    GLint value = 0;
    if (gGetIntegerv != nullptr) {
        gGetIntegerv(pname, &value);
    }
    return value;
}

unsigned DrainErrors() {
    if (gGetError == nullptr) {
        return 0;
    }
    unsigned count = 0;
    while (gGetError() != GL_NO_ERROR) {
        if (++count > 64) {
            break;
        }
    }
    return count;
}

bool MakeDirectories(const std::string &path) {
    if (path.empty()) {
        return false;
    }
    std::string partial;
    partial.reserve(path.size());
    for (std::size_t i = 0; i < path.size(); ++i) {
        partial.push_back(path[i]);
        const bool last = i + 1 == path.size();
#if defined(_WIN32)
        const bool separator = path[i] == '/' || path[i] == '\\';
#else
        const bool separator = path[i] == '/';
#endif
        if (!separator && !last) {
            continue;
        }
        if (partial == "/") {
            continue;
        }
#if defined(_WIN32)
        if (separator && partial.size() == 3 && partial[1] == ':') {
            continue;
        }
#endif
#if defined(_WIN32)
        if (_mkdir(partial.c_str()) != 0 && errno != EEXIST) {
#else
        if (mkdir(partial.c_str(), 0755) != 0 && errno != EEXIST) {
#endif
            return false;
        }
    }
    return true;
}

std::vector<std::string> Split(const std::string &value, char separator) {
    std::vector<std::string> parts;
    std::string current;
    for (const char c : value) {
        if (c == separator) {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    parts.push_back(current);
    return parts;
}

// CALL:DIR[:FBO,FBO,...] entries, separated by ';'. An omitted or `all` framebuffer list
// dumps every live framebuffer object.
void ParseDumpPoints(const char *spec) {
    for (const std::string &entry : Split(spec, ';')) {
        if (entry.empty()) {
            continue;
        }
        const std::vector<std::string> fields = Split(entry, ':');
        if (fields.size() < 2 || fields[0].empty() || fields[1].empty()) {
            std::cerr << "warning: ignoring malformed " << kDumpPointsEnv << " entry: " << entry << "\n";
            continue;
        }

        DumpPoint point;
        point.call = static_cast<unsigned>(std::strtoul(fields[0].c_str(), nullptr, 10));
        point.directory = fields[1];
        if (fields.size() >= 3 && !fields[2].empty() && fields[2] != "all") {
            for (const std::string &name : Split(fields[2], ',')) {
                if (!name.empty()) {
                    point.framebuffers.push_back(
                            static_cast<unsigned>(std::strtoul(name.c_str(), nullptr, 10)));
                }
            }
        }
        gDumpPoints.push_back(point);
    }
}

bool IsDecimal(const std::string &value) {
    return !value.empty() && value.find_first_not_of("0123456789") == std::string::npos;
}

void ParseTexture2dDumpPoints(const char *spec) {
    for (const std::string &entry : Split(spec, ';')) {
        if (entry.empty()) {
            continue;
        }
        const std::vector<std::string> fields = Split(entry, ',');
        if (fields.size() != 4 || !IsDecimal(fields[0]) || !IsDecimal(fields[1]) ||
            !IsDecimal(fields[2]) || fields[3].empty()) {
            std::cerr << "warning: ignoring malformed " << kTexture2dDumpPointsEnv
                      << " entry: " << entry << "\n";
            continue;
        }

        char *end = nullptr;
        const unsigned long call = std::strtoul(fields[0].c_str(), &end, 10);
        if (*end != '\0' || call > std::numeric_limits<unsigned>::max()) {
            std::cerr << "warning: ignoring malformed " << kTexture2dDumpPointsEnv
                      << " call: " << entry << "\n";
            continue;
        }
        const unsigned long texture = std::strtoul(fields[1].c_str(), &end, 10);
        if (*end != '\0' || texture == 0 || texture > std::numeric_limits<unsigned>::max()) {
            std::cerr << "warning: ignoring malformed " << kTexture2dDumpPointsEnv
                      << " texture: " << entry << "\n";
            continue;
        }
        const unsigned long level = std::strtoul(fields[2].c_str(), &end, 10);
        if (*end != '\0' || level > static_cast<unsigned long>(std::numeric_limits<GLint>::max())) {
            std::cerr << "warning: ignoring malformed " << kTexture2dDumpPointsEnv
                      << " level: " << entry << "\n";
            continue;
        }

        Texture2dDumpPoint point;
        point.call = static_cast<unsigned>(call);
        point.texture = static_cast<unsigned>(texture);
        point.level = static_cast<unsigned>(level);
        point.directory = fields[3];
        gTexture2dDumpPoints.push_back(point);
    }
}

unsigned ScanLimit() {
    const char *value = std::getenv(kScanLimitEnv);
    if (value == nullptr || value[0] == '\0') {
        return kDefaultScanLimit;
    }
    const unsigned limit = static_cast<unsigned>(std::strtoul(value, nullptr, 10));
    return limit == 0 ? kDefaultScanLimit : limit;
}

const char *ComponentTypeName(GLint componentType) {
    switch (componentType) {
    case GL_FLOAT:
        return "float";
    case GL_INT:
        return "int";
    case GL_UNSIGNED_INT:
        return "uint";
    case GL_SIGNED_NORMALIZED:
        return "snorm";
    case GL_UNSIGNED_NORMALIZED:
        return "unorm";
    case GL_NONE:
        return "none";
    default:
        return "unknown";
    }
}

bool DescribeAttachment(GLenum attachment, AttachmentDesc &desc) {
    glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, attachment,
                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &desc.objectType);
    if (DrainErrors() != 0 || desc.objectType == GL_NONE) {
        return false;
    }

    glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, attachment,
                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &desc.objectName);
    glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, attachment,
                                          GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &desc.componentType);
    DrainErrors();

    if (desc.objectType == GL_RENDERBUFFER) {
        const GLint boundRenderbuffer = GetInteger(GL_RENDERBUFFER_BINDING);
        glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(desc.objectName));
        glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &desc.width);
        glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &desc.height);
        glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_INTERNAL_FORMAT, &desc.internalFormat);
        glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(boundRenderbuffer));
    } else if (desc.objectType == GL_TEXTURE) {
        glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, attachment,
                                              GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL, &desc.level);
        glGetTextureLevelParameteriv(static_cast<GLuint>(desc.objectName), desc.level,
                                     GL_TEXTURE_WIDTH, &desc.width);
        glGetTextureLevelParameteriv(static_cast<GLuint>(desc.objectName), desc.level,
                                     GL_TEXTURE_HEIGHT, &desc.height);
        glGetTextureLevelParameteriv(static_cast<GLuint>(desc.objectName), desc.level,
                                     GL_TEXTURE_INTERNAL_FORMAT, &desc.internalFormat);
        if (DrainErrors() != 0 || desc.width <= 0 || desc.height <= 0) {
            // No direct-state-access level query: fall back to the classic bound query,
            // which only covers GL_TEXTURE_2D but is what render targets normally are.
            const GLint boundTexture = GetInteger(GL_TEXTURE_BINDING_2D);
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(desc.objectName));
            glGetTexLevelParameteriv(GL_TEXTURE_2D, desc.level, GL_TEXTURE_WIDTH, &desc.width);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, desc.level, GL_TEXTURE_HEIGHT, &desc.height);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, desc.level, GL_TEXTURE_INTERNAL_FORMAT,
                                     &desc.internalFormat);
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(boundTexture));
        }
    }

    DrainErrors();
    return desc.width > 0 && desc.height > 0;
}

bool DescribeTexture2D(GLuint texture, GLint level, AttachmentDesc &desc) {
    const GLint savedTexture = GetInteger(GL_TEXTURE_BINDING_2D);
    glBindTexture(GL_TEXTURE_2D, texture);
    desc.objectType = GL_TEXTURE;
    desc.objectName = static_cast<GLint>(texture);
    desc.level = level;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_WIDTH, &desc.width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_HEIGHT, &desc.height);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_INTERNAL_FORMAT, &desc.internalFormat);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_RED_TYPE, &desc.componentType);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(savedTexture));
    return DrainErrors() == 0 && desc.width > 0 && desc.height > 0;
}

bool ReadTexture2DFloats(const AttachmentDesc &desc, std::vector<float> &pixels) {
    const std::size_t count = static_cast<std::size_t>(desc.width) * desc.height * 4;
    pixels.assign(count, 0.0f);
    const GLint savedTexture = GetInteger(GL_TEXTURE_BINDING_2D);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(desc.objectName));
    if (desc.componentType == GL_INT || desc.componentType == GL_UNSIGNED_INT) {
        std::vector<std::int32_t> raw(count, 0);
        const GLenum type = desc.componentType == GL_INT ? GL_INT : GL_UNSIGNED_INT;
        glGetTexImage(GL_TEXTURE_2D, desc.level, GL_RGBA_INTEGER, type, raw.data());
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(savedTexture));
        if (DrainErrors() != 0) {
            return false;
        }
        for (std::size_t i = 0; i < count; ++i) {
            pixels[i] = desc.componentType == GL_INT
                                ? static_cast<float>(raw[i])
                                : static_cast<float>(static_cast<std::uint32_t>(raw[i]));
        }
        return true;
    }
    glGetTexImage(GL_TEXTURE_2D, desc.level, GL_RGBA, GL_FLOAT, pixels.data());
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(savedTexture));
    return DrainErrors() == 0;
}

// Reads the attachment as floats regardless of its storage: normalised and float targets
// convert on the way out, integer targets are read as integers and widened. The float view
// keeps out-of-[0,1] accumulation buffers legible in the statistics even though the PNG
// itself has to clamp.
bool ReadAttachmentFloats(const AttachmentDesc &desc, bool depth, unsigned channels,
                          std::vector<float> &pixels) {
    const std::size_t count = static_cast<std::size_t>(desc.width) * desc.height * channels;
    pixels.assign(count, 0.0f);

    if (depth) {
        glReadPixels(0, 0, desc.width, desc.height, GL_DEPTH_COMPONENT, GL_FLOAT, pixels.data());
        return DrainErrors() == 0;
    }

    if (desc.componentType == GL_INT || desc.componentType == GL_UNSIGNED_INT) {
        std::vector<std::int32_t> raw(count, 0);
        const GLenum type = desc.componentType == GL_INT ? GL_INT : GL_UNSIGNED_INT;
        glReadPixels(0, 0, desc.width, desc.height, GL_RGBA_INTEGER, type, raw.data());
        if (DrainErrors() != 0) {
            return false;
        }
        for (std::size_t i = 0; i < count; ++i) {
            pixels[i] = desc.componentType == GL_INT
                                ? static_cast<float>(raw[i])
                                : static_cast<float>(static_cast<std::uint32_t>(raw[i]));
        }
        return true;
    }

    glReadPixels(0, 0, desc.width, desc.height, GL_RGBA, GL_FLOAT, pixels.data());
    return DrainErrors() == 0;
}

std::string FormatStatistics(const std::vector<float> &pixels, unsigned channels) {
    float minimum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float maximum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    double total[4] = {0.0, 0.0, 0.0, 0.0};
    bool seeded[4] = {false, false, false, false};
    std::uint64_t hash = 1469598103934665603ull;
    std::size_t nonFinite = 0;

    const std::size_t pixelCount = channels == 0 ? 0 : pixels.size() / channels;
    for (std::size_t p = 0; p < pixelCount; ++p) {
        for (unsigned c = 0; c < channels; ++c) {
            const float value = pixels[p * channels + c];
            if (!std::isfinite(value)) {
                ++nonFinite;
                continue;
            }
            if (!seeded[c] || value < minimum[c]) {
                minimum[c] = value;
            }
            if (!seeded[c] || value > maximum[c]) {
                maximum[c] = value;
            }
            seeded[c] = true;
            total[c] += value;
        }
    }
    for (const float value : pixels) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        hash = (hash ^ bits) * 1099511628211ull;
    }

    char buffer[512];
    std::string text;
    for (unsigned c = 0; c < channels; ++c) {
        const double mean = pixelCount == 0 ? 0.0 : total[c] / static_cast<double>(pixelCount);
        std::snprintf(buffer, sizeof(buffer), " c%u[min=%.6g max=%.6g mean=%.6g]", c,
                      static_cast<double>(minimum[c]), static_cast<double>(maximum[c]), mean);
        text += buffer;
    }
    if (pixelCount > 0) {
        text += " first=(";
        for (unsigned c = 0; c < channels; ++c) {
            if (c > 0) {
                text += ",";
            }
            std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(pixels[c]));
            text += buffer;
        }
        text += ")";
    }
    std::snprintf(buffer, sizeof(buffer), " nonfinite=%zu hash=%016llx", nonFinite,
                  static_cast<unsigned long long>(hash));
    text += buffer;
    return text;
}

bool WriteFloatPng(const std::string &path, const AttachmentDesc &desc, unsigned channels,
                   const std::vector<float> &pixels) {
    image::Image snapshot(static_cast<unsigned>(desc.width), static_cast<unsigned>(desc.height),
                          channels, true, image::TYPE_FLOAT);
    if (snapshot.sizeInBytes() != pixels.size() * sizeof(float)) {
        return false;
    }
    std::memcpy(snapshot.pixels, pixels.data(), pixels.size() * sizeof(float));
    return snapshot.writePNG(path.c_str());
}

void DumpOneAttachment(std::ofstream &manifest, const std::string &directory,
                       const std::string &identity, GLenum attachment, const char *label, bool depth) {
    AttachmentDesc desc;
    if (!DescribeAttachment(attachment, desc)) {
        return;
    }

    const unsigned channels = depth ? 1u : 4u;
    if (!depth) {
        glReadBuffer(attachment);
        if (DrainErrors() != 0) {
            return;
        }
    }

    std::vector<float> pixels;
    const bool read = ReadAttachmentFloats(desc, depth, channels, pixels);

    const std::string path = directory + "/" + identity + "-" + label + ".png";
    const bool wrote = read && WriteFloatPng(path, desc, channels, pixels);

    char header[512];
    std::snprintf(header, sizeof(header),
                  "%s %s object=%s name=%d level=%d size=%dx%d internalformat=0x%04x component=%s",
                  identity.c_str(), label,
                  desc.objectType == GL_RENDERBUFFER ? "renderbuffer" : "texture", desc.objectName,
                  desc.level, desc.width, desc.height, static_cast<unsigned>(desc.internalFormat),
                  ComponentTypeName(desc.componentType));
    manifest << header;
    if (read) {
        manifest << FormatStatistics(pixels, channels);
    } else {
        manifest << " read=failed";
    }
    if (!wrote) {
        manifest << " png=failed";
    }
    manifest << "\n";
}

void DumpFramebuffer(std::ofstream &manifest, const std::string &directory, unsigned framebuffer,
                     GLint maxColorAttachments) {
    if (framebuffer == 0) {
        // The default framebuffer names its attachments GL_BACK_LEFT rather than
        // GL_COLOR_ATTACHMENT0, and the replay already snapshots it into actual.<call>.png.
        manifest << "fbo 0 skipped=default-framebuffer\n";
        return;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    if (DrainErrors() != 0) {
        return;
    }

    const GLint savedReadBuffer = GetInteger(GL_READ_BUFFER);
    const std::string identity = "fbo" + std::to_string(framebuffer);
    for (GLint index = 0; index < maxColorAttachments; ++index) {
        char label[32];
        std::snprintf(label, sizeof(label), "att%d", index);
        DumpOneAttachment(manifest, directory, identity,
                          static_cast<GLenum>(GL_COLOR_ATTACHMENT0 + index), label, false);
    }
    DumpOneAttachment(manifest, directory, identity, GL_DEPTH_ATTACHMENT, "depth", true);

    // The read buffer is per-framebuffer state the trace goes on using; put it back.
    if (framebuffer != 0 && savedReadBuffer != 0) {
        glReadBuffer(static_cast<GLenum>(savedReadBuffer));
        DrainErrors();
    }
}

void RunDumpPoint(DumpPoint &point) {
    if (!MakeDirectories(point.directory)) {
        std::cerr << "warning: failed to create FBO dump directory " << point.directory << "\n";
        point.done = true;
        return;
    }

    // Start from a clean error state so a failure reported below is one we caused.
    DrainErrors();

    // Everything below perturbs read-side and pack state; snapshot it so the replay
    // continues from where it was.
    const GLint savedReadFramebuffer = GetInteger(GL_READ_FRAMEBUFFER_BINDING);
    const GLint savedPackBuffer = GetInteger(GL_PIXEL_PACK_BUFFER_BINDING);
    const GLint savedPackAlignment = GetInteger(GL_PACK_ALIGNMENT);
    const GLint savedPackRowLength = GetInteger(GL_PACK_ROW_LENGTH);
    const GLint savedPackSkipPixels = GetInteger(GL_PACK_SKIP_PIXELS);
    const GLint savedPackSkipRows = GetInteger(GL_PACK_SKIP_ROWS);
    const GLint savedPackImageHeight = GetInteger(GL_PACK_IMAGE_HEIGHT);
    const GLint savedPackSkipImages = GetInteger(GL_PACK_SKIP_IMAGES);
    const GLint savedPackSwapBytes = GetInteger(GL_PACK_SWAP_BYTES);
    DrainErrors();

    if (savedPackBuffer != 0) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_IMAGE_HEIGHT, 0);
    glPixelStorei(GL_PACK_SKIP_IMAGES, 0);
    glPixelStorei(GL_PACK_SWAP_BYTES, GL_FALSE);
    DrainErrors();

    const GLint maxColorAttachments = GetInteger(GL_MAX_COLOR_ATTACHMENTS);
    DrainErrors();

    std::vector<unsigned> framebuffers = point.framebuffers;
    if (framebuffers.empty()) {
        const unsigned limit = ScanLimit();
        for (unsigned name = 1; name <= limit; ++name) {
            if (glIsFramebuffer(name) == GL_TRUE) {
                framebuffers.push_back(name);
            }
        }
        DrainErrors();
    }

    const std::string manifestPath = point.directory + "/manifest.txt";
    std::ofstream manifest(manifestPath, std::ios::trunc);
    manifest << "call " << retrace::callNo << " framebuffers " << framebuffers.size()
             << " maxcolorattachments " << maxColorAttachments << "\n";
    for (const unsigned framebuffer : framebuffers) {
        DumpFramebuffer(manifest, point.directory, framebuffer, maxColorAttachments);
    }
    manifest.flush();

    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(savedReadFramebuffer));
    if (savedPackBuffer != 0) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(savedPackBuffer));
    }
    glPixelStorei(GL_PACK_ALIGNMENT, savedPackAlignment);
    glPixelStorei(GL_PACK_ROW_LENGTH, savedPackRowLength);
    glPixelStorei(GL_PACK_SKIP_PIXELS, savedPackSkipPixels);
    glPixelStorei(GL_PACK_SKIP_ROWS, savedPackSkipRows);
    glPixelStorei(GL_PACK_IMAGE_HEIGHT, savedPackImageHeight);
    glPixelStorei(GL_PACK_SKIP_IMAGES, savedPackSkipImages);
    glPixelStorei(GL_PACK_SWAP_BYTES, savedPackSwapBytes);
    DrainErrors();

    std::cerr << "MOBILEGL_TRACE_FBO_DUMP: call " << retrace::callNo << " -> " << manifestPath
              << " (" << framebuffers.size() << " framebuffers)\n";
    point.done = true;
}

void RunTexture2dDumpPoint(Texture2dDumpPoint &point) {
    if (!MakeDirectories(point.directory)) {
        std::cerr << "warning: failed to create texture dump directory " << point.directory << "\n";
        point.done = true;
        return;
    }

    // glGetError is destructive. This debug-only snapshot hook deliberately starts from a
    // clean error state so diagnostics below identify the dump rather than an earlier trace call.
    DrainErrors();
    const GLint savedReadFramebuffer = GetInteger(GL_READ_FRAMEBUFFER_BINDING);
    const GLint savedPackBuffer = GetInteger(GL_PIXEL_PACK_BUFFER_BINDING);
    const GLint savedPackAlignment = GetInteger(GL_PACK_ALIGNMENT);
    const GLint savedPackRowLength = GetInteger(GL_PACK_ROW_LENGTH);
    const GLint savedPackSkipPixels = GetInteger(GL_PACK_SKIP_PIXELS);
    const GLint savedPackSkipRows = GetInteger(GL_PACK_SKIP_ROWS);
    const GLint savedPackImageHeight = GetInteger(GL_PACK_IMAGE_HEIGHT);
    const GLint savedPackSkipImages = GetInteger(GL_PACK_SKIP_IMAGES);
    const GLint savedPackSwapBytes = GetInteger(GL_PACK_SWAP_BYTES);
    DrainErrors();

    if (savedPackBuffer != 0) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_IMAGE_HEIGHT, 0);
    glPixelStorei(GL_PACK_SKIP_IMAGES, 0);
    glPixelStorei(GL_PACK_SWAP_BYTES, GL_FALSE);
    DrainErrors();

    const std::string identity = "texture" + std::to_string(point.texture) +
                                 "-level" + std::to_string(point.level);
    const std::string manifestPath = point.directory + "/manifest.txt";
    std::ofstream manifest(manifestPath, std::ios::trunc);
    manifest << "call " << retrace::callNo << " texture " << point.texture << " level "
             << point.level << "\n";
    AttachmentDesc desc;
    if (glIsTexture(point.texture) == GL_FALSE ||
        !DescribeTexture2D(point.texture, static_cast<GLint>(point.level), desc)) {
        manifest << identity << " skipped=not-live-2d-texture\n";
    } else {
        std::vector<float> pixels;
        const bool read = ReadTexture2DFloats(desc, pixels);
        const bool wrote = read && WriteFloatPng(point.directory + "/" + identity + ".png", desc, 4, pixels);
        manifest << identity << " object=texture name=" << desc.objectName << " level=" << desc.level
                 << " size=" << desc.width << "x" << desc.height << " internalformat=0x" << std::hex
                 << static_cast<unsigned>(desc.internalFormat) << std::dec
                 << " component=" << ComponentTypeName(desc.componentType);
        if (read) {
            manifest << FormatStatistics(pixels, 4);
        } else {
            manifest << " read=failed";
        }
        if (!wrote) {
            manifest << " png=failed";
        }
        manifest << "\n";
    }
    manifest.flush();

    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(savedReadFramebuffer));
    if (savedPackBuffer != 0) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(savedPackBuffer));
    }
    glPixelStorei(GL_PACK_ALIGNMENT, savedPackAlignment);
    glPixelStorei(GL_PACK_ROW_LENGTH, savedPackRowLength);
    glPixelStorei(GL_PACK_SKIP_PIXELS, savedPackSkipPixels);
    glPixelStorei(GL_PACK_SKIP_ROWS, savedPackSkipRows);
    glPixelStorei(GL_PACK_IMAGE_HEIGHT, savedPackImageHeight);
    glPixelStorei(GL_PACK_SKIP_IMAGES, savedPackSkipImages);
    glPixelStorei(GL_PACK_SWAP_BYTES, savedPackSwapBytes);
    DrainErrors();

    std::cerr << "MOBILEGL_TRACE_TEXTURE_2D_DUMP: call " << retrace::callNo << " texture "
              << point.texture << " level " << point.level << " -> " << manifestPath << "\n";
    point.done = true;
}

void RunPendingDumps() {
    for (DumpPoint &point : gDumpPoints) {
        if (!point.done && point.call == retrace::callNo) {
            RunDumpPoint(point);
        }
    }
    for (Texture2dDumpPoint &point : gTexture2dDumpPoints) {
        if (!point.done && point.call == retrace::callNo) {
            RunTexture2dDumpPoint(point);
        }
    }
}

class DumpingDumper final : public retrace::Dumper {
public:
    int getSnapshotCount(void) override {
        RunPendingDumps();
        return gInnerDumper->getSnapshotCount();
    }

    image::Image *getSnapshot(int n, bool backBuffer) override {
        return gInnerDumper->getSnapshot(n, backBuffer);
    }

    bool canDump(void) override {
        return gInnerDumper->canDump();
    }

    void dumpState(StateWriter &writer) override {
        gInnerDumper->dumpState(writer);
    }
};

DumpingDumper gDumpingDumper;

} // namespace

void InstallIfRequested() {
    if (gInstalled) {
        return;
    }
    if (!gConfigured) {
        gConfigured = true;
        const char *spec = std::getenv(kDumpPointsEnv);
        if (spec != nullptr && spec[0] != '\0') {
            ParseDumpPoints(spec);
        }
        const char *textureSpec = std::getenv(kTexture2dDumpPointsEnv);
        if (textureSpec != nullptr && textureSpec[0] != '\0') {
            ParseTexture2dDumpPoints(textureSpec);
        }
    }
    if (gDumpPoints.empty() && gTexture2dDumpPoints.empty()) {
        gInstalled = true;
        return;
    }
    if (retrace::dumper == nullptr || retrace::dumper == &gDumpingDumper) {
        return;
    }

    ResolveDirectEntryPoints();
    gInnerDumper = retrace::dumper;
    retrace::dumper = &gDumpingDumper;
    gInstalled = true;
    for (const DumpPoint &point : gDumpPoints) {
        std::cerr << "MOBILEGL_TRACE_FBO_DUMP: armed for call " << point.call << " -> "
                  << point.directory << "\n";
    }
    for (const Texture2dDumpPoint &point : gTexture2dDumpPoints) {
        std::cerr << "MOBILEGL_TRACE_TEXTURE_2D_DUMP: armed for call " << point.call
                  << " texture " << point.texture << " level " << point.level << " -> "
                  << point.directory << "\n";
    }
}

} // namespace mobilegl_trace_dump
