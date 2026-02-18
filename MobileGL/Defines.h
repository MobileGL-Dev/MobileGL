// MobileGL - MobileGL/Defines.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

// ============== Platform-specific definitions and macros ============== //
#ifdef __ANDROID__
#undef __ANDROID_API__
#define __ANDROID_API__ 26 // force Android API level to 26 for compatibility
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX 1 // prevent Windows.h from defining min and max macros
#endif
#endif

// ================== MobileGL definitions and macros =================== //
#ifdef _WIN32
#define MOBILEGL_EXPORT extern "C" __declspec(dllexport)
#else
#define MOBILEGL_EXPORT extern "C" __attribute__((visibility("default")))
#endif

#define MOBILEGL_API MOBILEGL_EXPORT

#define MOBILEGL_GLX_API MOBILEGL_API
#define MOBILEGL_GL_API MOBILEGL_API
#define MOBILEGL_EGL_API MOBILEGL_API

// ====================== MobileGL configurations ======================= //
#define MOBILEGL_LOG_ACTIVE_LEVEL MOBILEGL_LOG_LEVEL_INFO

#define MOBILEGL_LOG_ENABLE_CONSOLE 0
#define MOBILEGL_LOG_ENABLE_FILE 1
#define MOBILEGL_LOG_ENABLE_ANDROID 1
#define MOBILEGL_ENABLE_SCOPE_MARKER 0

// Require C++23
// Clang/Android NDK still doesn't have support for that :(
#if __cplusplus >= 202302L && !__ANDROID__
#define MOBILEGL_LOG_ENABLE_STACKTRACE 0
#endif

#ifdef __ANDROID__
#define MOBILEGL_LOG_FILE_PATH "/sdcard/MG/latest.log"
#else
#define MOBILEGL_LOG_FILE_PATH ""
#endif

#if defined _MSC_VER or defined __MINGW32__ or defined __MINGW64__
#define TRAP assert(false)
#elif __clang__
#define TRAP __builtin_debugtrap()
#else
#include <signal.h>
#define TRAP raise(SIGTRAP)
#endif

// =============================== Utils ================================ //
#define MOBILEGL_ASSERT(condition, ...)                                                                                \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            MGLOG_F("Assertion failed" __VA_OPT__(": ") __VA_ARGS__);                                                  \
            MGLOG_F("  at %s:%d (%s)", __FILE__, __LINE__, __func__);                                                  \
            TRAP;                                                                                                      \
        }                                                                                                              \
    } while (0)
