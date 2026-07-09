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

#include <jni.h>

namespace {
    using MobileGL::Bool;
    using MobileGL::SizeT;
    using MobileGL::String;
    using MobileGL::StringStream;
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
        out << "]}";
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
