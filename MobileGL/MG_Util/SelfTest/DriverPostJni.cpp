// MobileGL - MobileGL/MG_Util/SelfTest/DriverPostJni.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// JNI surface for the plugin APK's driver POST screen. The CMake source list only compiles
// this file on Android; the guard keeps it inert if it ever ends up in another build.
#ifdef __ANDROID__

#include "DriverPost.h"
#include <MG_Util/Converters/MGToStr/TextureEnumConverter.h>

#include <jni.h>

namespace {
    using MobileGL::Bool;
    using MobileGL::SizeT;
    using MobileGL::String;
    using MobileGL::StringStream;
    using MobileGL::Uint64;
    using MobileGL::MG_Backend::FormatCapabilityCache;
    using MobileGL::MG_Backend::FormatCapabilityFlags;
    using MobileGL::MG_Util::SelfTest::BackendPostReport;
    using MobileGL::MG_Util::SelfTest::PostCheck;

    String EscapeJsonString(const String& value) {
        String out;
        out.reserve(value.size() + 8);
        for (const char c : value) {
            switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                // Bytes outside printable ASCII are \u00XX-escaped too: driver strings can carry
                // arbitrary bytes, and NewStringUTF aborts under CheckJNI when the payload is not
                // valid Modified UTF-8. Escaping keeps the JSON pure ASCII.
                if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) >= 0x7F) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buffer;
                } else {
                    out += c;
                }
                break;
            }
        }
        return out;
    }

    void AppendJsonString(StringStream& out, const String& value) {
        out << '"' << EscapeJsonString(value) << '"';
    }

    Uint64 BuildFormatCapabilityMask(FormatCapabilityFlags capabilities) {
        Uint64 mask = 0;
        for (SizeT capabilityIndex = 0;
             capabilityIndex < MobileGL::MG_Backend::kReportedFormatCapabilities.size(); ++capabilityIndex) {
            if (MobileGL::MG_Backend::HasFormatCapability(
                    capabilities, MobileGL::MG_Backend::kReportedFormatCapabilities[capabilityIndex])) {
                mask |= 1ull << capabilityIndex;
            }
        }
        return mask;
    }

    void AppendFormatCapabilitiesJson(StringStream& out, const FormatCapabilityCache& cache) {
        out << ",\"formatCapabilities\":{\"capabilities\":[";
        for (SizeT capabilityIndex = 0;
             capabilityIndex < MobileGL::MG_Backend::kReportedFormatCapabilities.size(); ++capabilityIndex) {
            if (capabilityIndex != 0) {
                out << ',';
            }
            AppendJsonString(
                out, MobileGL::MG_Backend::GetFormatCapabilityName(
                         MobileGL::MG_Backend::kReportedFormatCapabilities[capabilityIndex]));
        }
        out << "],\"targets\":[";
        for (SizeT targetIndex = 0; targetIndex < MobileGL::MG_Backend::kFormatCapabilityTargetCount;
             ++targetIndex) {
            if (targetIndex != 0) {
                out << ',';
            }
            out << "{\"name\":";
            AppendJsonString(out, MobileGL::MG_Backend::GetFormatCapabilityTargetName(targetIndex));
            out << ",\"rows\":[";
            for (SizeT formatIndex = 0; formatIndex < MobileGL::MG_Backend::kFormatCapabilityFormatCount;
                 ++formatIndex) {
                if (formatIndex != 0) {
                    out << ',';
                }
                out << '[';
                AppendJsonString(
                    out, MobileGL::MG_Util::ConvertTextureInternalFormatToString(
                             static_cast<MobileGL::TextureInternalFormat>(formatIndex)));
                out << ',' << BuildFormatCapabilityMask(cache.FullCaps[targetIndex][formatIndex]);
                out << ',' << BuildFormatCapabilityMask(cache.CaveatCaps[targetIndex][formatIndex]) << ']';
            }
            out << "]}";
        }
        out << "]}";
    }

    void AppendBackendReportJson(StringStream& out, const BackendPostReport& report) {
        out << "{\"available\":" << (report.available ? "true" : "false");
        out << ",\"verdict\":";
        AppendJsonString(out, report.verdict);
        out << ",\"renderer\":";
        AppendJsonString(out, report.rendererInfo);
        out << ",\"checks\":[";
        for (SizeT i = 0; i < report.checks.size(); ++i) {
            const PostCheck& check = report.checks[i];
            if (i != 0) {
                out << ',';
            }
            out << "{\"name\":";
            AppendJsonString(out, check.name);
            out << ",\"status\":";
            AppendJsonString(out, check.status);
            out << ",\"detail\":";
            AppendJsonString(out, check.detail);
            out << '}';
        }
        out << ']';
        // The "Known Driver Bugs" section, separate from "checks" because it answers a
        // different question and uses a different verdict vocabulary (FIXED | UNFIXABLE).
        // Every entry is a bug the device HAS - a clean probe contributes nothing - so an
        // unaffected driver serializes an empty array and the screen renders no section.
        out << ",\"knownDriverBugs\":[";
        for (SizeT i = 0; i < report.knownDriverBugs.size(); ++i) {
            const MobileGL::MG_Util::SelfTest::DriverBugFinding& bug = report.knownDriverBugs[i];
            if (i != 0) {
                out << ',';
            }
            out << "{\"name\":";
            AppendJsonString(out, bug.name);
            out << ",\"verdict\":";
            AppendJsonString(out, bug.verdict == MobileGL::MG_Util::SelfTest::DriverBugVerdict::Fixed
                                      ? "FIXED"
                                      : "UNFIXABLE");
            out << ",\"detail\":";
            AppendJsonString(out, bug.detail);
            out << '}';
        }
        out << ']';
        if (report.formatCapabilities.has_value()) {
            AppendFormatCapabilitiesJson(out, report.formatCapabilities.value());
        }
        out << '}';
    }
} // namespace

extern "C" JNIEXPORT jstring JNICALL Java_top_mobilegl_plugin_PostActivity_nativeRunDriverPost(JNIEnv* env, jclass) {
    String json;
    try {
        const BackendPostReport glesReport = MobileGL::MG_Util::SelfTest::RunGlesDriverPost();
        const BackendPostReport vulkanReport = MobileGL::MG_Util::SelfTest::RunVulkanDriverPost();
        StringStream out;
        out << "{\"gles\":";
        AppendBackendReportJson(out, glesReport);
        out << ",\"vulkan\":";
        AppendBackendReportJson(out, vulkanReport);
        out << '}';
        json = out.str();
    } catch (const std::exception& exception) {
        json = "{\"error\":\"" + EscapeJsonString(exception.what()) + "\"}";
    } catch (...) {
        json = "{\"error\":\"driver POST failed with an unknown native exception\"}";
    }
    return env->NewStringUTF(json.c_str());
}

#endif // __ANDROID__
