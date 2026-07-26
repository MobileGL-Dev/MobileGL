// MobileGL - MobileGL/MG_Util/Debug/Log.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

// Severity-ordered: a build compiled at level X keeps X and everything MORE
// severe. INFO builds must keep WARN/ERROR/FATAL — the old ordering
// (WARN=1/ERROR=2 below INFO=3) compiled every warning and error out of
// release builds and hid real backend failures.
#define MOBILEGL_LOG_LEVEL_DEBUG 0
#define MOBILEGL_LOG_LEVEL_INFO 1
#define MOBILEGL_LOG_LEVEL_WARN 2
#define MOBILEGL_LOG_LEVEL_ERROR 3
#define MOBILEGL_LOG_LEVEL_FATAL 4

#define MOBILEGL_LOG_INTERNAL(levelTag, androidLogLevel, fmt, ...)                                                     \
    do {                                                                                                               \
        MobileGL::MG_Util::Debug::Log(levelTag, androidLogLevel, fmt, ##__VA_ARGS__);                                  \
    } while (0)

#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG
#define MGLOG_D(fmt, ...) MOBILEGL_LOG_INTERNAL("DEBUG", ANDROID_LOG_DEBUG, fmt, ##__VA_ARGS__)
#else
#define MGLOG_D(fmt, ...)                                                                                              \
    {}
#endif

#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_INFO
#define MGLOG_I(fmt, ...) MOBILEGL_LOG_INTERNAL("INFO", ANDROID_LOG_INFO, fmt, ##__VA_ARGS__)
#else
#define MGLOG_I(fmt, ...)                                                                                              \
    {}
#endif

#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_WARN
#define MGLOG_W(fmt, ...) MOBILEGL_LOG_INTERNAL("WARN", ANDROID_LOG_WARN, fmt, ##__VA_ARGS__)
#else
#define MGLOG_W(fmt, ...)                                                                                              \
    {}
#endif

#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_ERROR
#define MGLOG_E(fmt, ...) MOBILEGL_LOG_INTERNAL("ERROR", ANDROID_LOG_ERROR, fmt, ##__VA_ARGS__)
#else
#define MGLOG_E(fmt, ...)                                                                                              \
    {}
#endif

#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_FATAL
#define MGLOG_F(fmt, ...) MOBILEGL_LOG_INTERNAL("FATAL", ANDROID_LOG_FATAL, fmt, ##__VA_ARGS__)
#else
#define MGLOG_F(fmt, ...)                                                                                              \
    {}
#endif

namespace MobileGL {
    namespace MG_Util {
        namespace Debug {
            void Log(const char* levelTag, android_LogPriority androidLogLevel, const char* fmt, ...);
            void WriteToFile(const char* msg);
            std::string GetCurrentTime();
            constexpr char* GetOSName();
            std::string GetThreadName();
            void Close();
        } // namespace Debug
    } // namespace MG_Util
} // namespace MobileGL
