// MobileGL - MobileGL/MG_Util/Debug/Log.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "../../Includes.h"

namespace MobileGL {
    namespace MG_Util::Debug {
        static FILE* s_logFile = nullptr;

        std::mutex& LogMutex() {
            static auto* mutex = new std::mutex();
            return *mutex;
        }

        constexpr char* GetOSName() {
#if defined(_WIN32)
            return (char*)"Windows";
#elif defined(__ANDROID__)
            return (char*)"Android";
#elif defined(__APPLE__)
            return (char*)"macOS";
#elif defined(__linux__)
            return (char*)"Linux";
#else
            return (char*)"UnknownOS";
#endif
        }

        std::string GetThreadName() {
            char buffer[64] = {0};
#if defined(_WIN32) && !defined(__MINGW32__)
            PWSTR desc = nullptr;
            if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &desc))) {
                WideCharToMultiByte(CP_UTF8, 0, desc, -1, buffer, sizeof(buffer), nullptr, nullptr);
                LocalFree(desc);
            }
#elif defined(__ANDROID__) || defined(__linux__) || defined(__APPLE__) || defined(__MINGW32__)
            pthread_getname_np(pthread_self(), buffer, sizeof(buffer));
#endif
            return buffer[0] ? buffer : "UnknownThread";
        }

        std::string GetCurrentTime() {
            using namespace std::chrono;
            auto now = system_clock::now();
            std::time_t tt = system_clock::to_time_t(now);
            struct tm tm{};
#if defined(_WIN32)
            localtime_s(&tm, &tt);
#else
            localtime_r(&tt, &tm);
#endif
            char buf[16];
            std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
            return buf;
        }

        void InitFile() {
#if MOBILEGL_LOG_ENABLE_FILE
            if (!s_logFile) {
                const char* logPath = std::getenv("MOBILEGL_LOG_FILE_PATH");
                if (!logPath || !*logPath) {
                    logPath = MOBILEGL_LOG_FILE_PATH;
                }
                if (logPath && *logPath) {
                    s_logFile = std::fopen(logPath, "w");
                }
            }
#endif
        }

        void WriteToFile(const char* msg) {
#if MOBILEGL_LOG_ENABLE_FILE
            if (!s_logFile) InitFile();
            if (s_logFile) {
                std::fputs(msg, s_logFile);
                std::fflush(s_logFile);
            }
#endif
        }

        void Log(const char* levelTag, android_LogPriority androidLogLevel, const char* fmt, ...) {
            std::lock_guard<std::mutex> lock(LogMutex());

#if MOBILEGL_LOG_ENABLE_STACKTRACE
            auto trace = std::stacktrace::current();
            std::string padding(4 * trace.size(), ' ');
#endif

            std::string header =
                "[" + GetCurrentTime() + "] [" + GetOSName() + " " + GetThreadName() + "/" + levelTag + "]: ";

            static char buffer[1024000];
            va_list args;
            va_start(args, fmt);
            int n = std::vsnprintf(buffer, sizeof(buffer), fmt, args);
            const SizeT messageLength =
                (n < 0) ? 0 : std::min(static_cast<SizeT>(n), sizeof(buffer) - static_cast<SizeT>(1));
            std::string out = header +
#if MOBILEGL_LOG_ENABLE_STACKTRACE
                              padding +
#endif
                              std::string(buffer, messageLength) + "\n";

#if MOBILEGL_LOG_ENABLE_CONSOLE
            std::fwrite(out.c_str(), 1, out.size(), stdout);
            fflush(stdout);
#endif

#if MOBILEGL_LOG_ENABLE_ANDROID && defined(__ANDROID__)
            __android_log_print(androidLogLevel, "MobileGL", "%s", out.c_str());
#endif

            WriteToFile(out.c_str());

            va_end(args);
        }

        void Close() {
#if MOBILEGL_LOG_ENABLE_FILE
            if (s_logFile) {
                std::fclose(s_logFile);
                s_logFile = nullptr;
            }
#endif
        }
    } // namespace MG_Util::Debug
} // namespace MobileGL
