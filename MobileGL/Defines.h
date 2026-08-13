// MobileGL - MobileGL/Defines.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

// ============== Platform-specific definitions and macros ============== //
// No __ANDROID_API__ pin here on purpose. The effective API level is owned by
// the build system (gradle minSdk 26 -> -DANDROID_PLATFORM=android-26, enforced
// by the configure-time guard in CMakeLists.txt), not by a macro.
//
// History: this used to `#define __ANDROID_API__ 26` to *raise* the level back
// when the build configured something lower, so that pthread_getname_np (which
// bionic guards with __INTRODUCED_IN(26)) would be declared. Once a later
// change added an `#undef` in front of it, the same line started *lowering* the
// level whenever the build configured higher than 26 - and that is an
// include-order split-brain, not a compatibility knob: a TU that includes any
// libc++ header before Includes.h latches libc++'s feature macros at the
// configure-time level, and only the bionic headers pulled in afterwards see
// the lowered value. The two halves then disagree (e.g. libc++ believes
// pthread_cond_clockwait exists while bionic has since hidden its declaration).

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
#define MOBILEGL_CGL_API MOBILEGL_API
#define MOBILEGL_NSOPENGL_API MOBILEGL_API
#define MOBILEGL_WGL_API MOBILEGL_API

// ====================== MobileGL configurations ======================= //
// The numeric log levels live here, not only in Log.h: MOBILEGL_ASSERT below compares
// MOBILEGL_LOG_ACTIVE_LEVEL against MOBILEGL_LOG_LEVEL_DEBUG, and in a translation unit
// that includes Defines.h without Log.h both tokens would silently evaluate to 0 in the
// preprocessor conditional - enabling the assert in exactly the INFO-level builds it is
// documented to be compiled out of. Log.h redefines them identically, which is legal.
//
// Severity order, ascending: DEBUG < INFO < WARN < ERROR < FATAL. MOBILEGL_LOG_ACTIVE_LEVEL
// names the lowest severity compiled in, so the production default INFO keeps I/W/E/F and
// drops only D. Any edit here must be mirrored in Log.h.
#ifndef MOBILEGL_LOG_LEVEL_DEBUG
#define MOBILEGL_LOG_LEVEL_DEBUG 0
#define MOBILEGL_LOG_LEVEL_INFO 1
#define MOBILEGL_LOG_LEVEL_WARN 2
#define MOBILEGL_LOG_LEVEL_ERROR 3
#define MOBILEGL_LOG_LEVEL_FATAL 4
#endif

#ifndef MOBILEGL_LOG_ACTIVE_LEVEL
#define MOBILEGL_LOG_ACTIVE_LEVEL MOBILEGL_LOG_LEVEL_INFO
#endif

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
// Asserts are live in exactly the builds where MGLOG_D is live, i.e. DEBUG builds only;
// an INFO build (the production default) compiles them out. DEBUG is the lowest severity
// in the ordering above, so "ACTIVE <= DEBUG" is true only for ACTIVE == DEBUG - the same
// gate MGLOG_D uses in Log.h. That equivalence is what makes this gate survive the
// 2026-08-13 renumbering unchanged; the contract is and stays
// "INFO builds: asserts OFF; DEBUG builds: asserts ON".
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG
    #define MOBILEGL_ASSERT(condition, ...)                                                                                \
        do {                                                                                                               \
            if (!(condition)) {                                                                                            \
                MGLOG_F("Assertion failed" __VA_OPT__(": ") __VA_ARGS__);                                                  \
                MGLOG_F("  at %s:%d (%s)", __FILE__, __LINE__, __func__);                                                  \
                TRAP;                                                                                                      \
            }                                                                                                              \
        } while (0)
#else
    #define MOBILEGL_ASSERT(condition, ...)
#endif
