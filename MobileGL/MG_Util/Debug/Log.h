// MobileGL - MobileGL/MG_Util/Debug/Log.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

#include <atomic>

// Severity order, ascending. MOBILEGL_LOG_ACTIVE_LEVEL names the LOWEST severity that is
// compiled in, so every level at or above it survives and everything below it becomes a
// no-op: the production default INFO admits I/W/E/F and drops only D.
//
// This ordering was inverted until 2026-08-13 (DEBUG < WARN < ERROR < INFO < FATAL), which
// silently compiled MGLOG_W and MGLOG_E out of every production and CI build and cost
// several real diagnostic blackouts. Do not reorder without re-reading every
// `#if MOBILEGL_LOG_ACTIVE_LEVEL <= ...` in the tree.
//
// These five constants are duplicated verbatim in Defines.h, which needs them for the
// MOBILEGL_ASSERT gate in translation units that do not include Log.h. Keep both copies
// in sync; the values are load-bearing, not cosmetic.
#define MOBILEGL_LOG_LEVEL_DEBUG 0
#define MOBILEGL_LOG_LEVEL_INFO 1
#define MOBILEGL_LOG_LEVEL_WARN 2
#define MOBILEGL_LOG_LEVEL_ERROR 3
#define MOBILEGL_LOG_LEVEL_FATAL 4

#define MOBILEGL_LOG_INTERNAL(levelTag, androidLogLevel, fmt, ...)                                                     \
    do {                                                                                                               \
        MobileGL::MG_Util::Debug::Log(levelTag, androidLogLevel, fmt, ##__VA_ARGS__);                                  \
    } while (0)

// Emit `inner` at most once per call site, for the life of the process.
//
// Production logging is not allowed to repeat: a diagnostic on a per-draw or per-frame
// path costs frame time on every occurrence and buries the rest of the log. Anything at
// W or E that sits on such a path must either be latched with one of the _ONCE forms
// below or be demoted to MGLOG_D, which production compiles out entirely.
//
// The latch is a function-local atomic - zero-initialised before any dynamic
// initialisation runs, so it is safe from any thread at any time, needs no guard
// variable, and costs one relaxed test-and-set on the already-cold failure path. Note
// that the latch is per CALL SITE, not per subject: a site that reports "texture %u is
// unsupported" reports only the first such texture. That is the intended trade - the
// first occurrence is what a user shares for troubleshooting, and MGLOG_D still shows
// every occurrence in a dev build.
#define MOBILEGL_LOG_ONCE_INTERNAL(inner, fmt, ...)                                                                    \
    do {                                                                                                               \
        static ::std::atomic_flag mobileglLogOnceLatch;                                                                \
        if (!mobileglLogOnceLatch.test_and_set(::std::memory_order_relaxed)) {                                         \
            inner(fmt, ##__VA_ARGS__);                                                                                 \
        }                                                                                                              \
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

// One-shot forms. Each is gated on its own level so that a suppressed level leaves no
// latch behind - MGLOG_D_ONCE in a production build is nothing at all, not a byte of
// state plus a test-and-set.
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG
#define MGLOG_D_ONCE(fmt, ...) MOBILEGL_LOG_ONCE_INTERNAL(MGLOG_D, fmt, ##__VA_ARGS__)
#else
#define MGLOG_D_ONCE(fmt, ...)                                                                                         \
    {}
#endif

#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_INFO
#define MGLOG_I_ONCE(fmt, ...) MOBILEGL_LOG_ONCE_INTERNAL(MGLOG_I, fmt, ##__VA_ARGS__)
#else
#define MGLOG_I_ONCE(fmt, ...)                                                                                         \
    {}
#endif

#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_WARN
#define MGLOG_W_ONCE(fmt, ...) MOBILEGL_LOG_ONCE_INTERNAL(MGLOG_W, fmt, ##__VA_ARGS__)
#else
#define MGLOG_W_ONCE(fmt, ...)                                                                                         \
    {}
#endif

#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_ERROR
#define MGLOG_E_ONCE(fmt, ...) MOBILEGL_LOG_ONCE_INTERNAL(MGLOG_E, fmt, ##__VA_ARGS__)
#else
#define MGLOG_E_ONCE(fmt, ...)                                                                                         \
    {}
#endif

#include <string>

namespace MobileGL {
    namespace MG_Util {
        namespace Debug {
            void Log(const char* levelTag, android_LogPriority androidLogLevel, const char* fmt, ...);
            void WriteToFile(const char* msg);
            std::string GetCurrentTime();
            constexpr char* GetOSName();
            std::string GetThreadName();
            void Close();

#if MOBILEGL_BUILD_DISAGGREGATED
            // P6: ONE LOG PER ROLE, and the role is a per-THREAD fact because under inproc both
            // roles live in one process. The client keeps MOBILEGL_LOG_FILE_PATH unchanged so
            // every existing reader keeps its path; the server's lines go to the same path with
            // `.server` inserted before the extension.
            //
            // WHY NOT ONE SHARED FILE. It worked - O_APPEND made the spawn case correct - but it
            // put two sessions' lines in one place and made every per-side assertion a search
            // rather than a read. The `--require-spawn` gate had to accept "ANY Config: IPC line
            // matches" because the server's own line is scrubbed of the client's knobs by
            // construction; with the two separated it can demand that THE client's line matches.
            //
            // THREAD-SCOPED, NOT PROCESS-SCOPED, for the case that is easy to miss: under inproc
            // the server role is the mgl-srv-apply THREAD, so a process-wide flag would file its
            // lines under whichever role set it last.
            enum class LogRole { Client, Server };

            // Called by the apply thread when it starts (inproc), and once by ServerMain for a
            // whole spawned server process. Without it a thread defaults to the process role,
            // which MOBILEGL_IPC_ROLE=server sets for the spawned image.
            void SetThreadLogRole(LogRole role);

            // THE NAMING RULE, EXPORTED, so that no reader has to reimplement it. Every consumer
            // of a lane's log - the unit mains, the integration harness, the retrace runner -
            // needs the same `<stem>.<role><ext>` derivation, and a second copy is a copy that
            // can fall behind: the one that does opens a file that does not exist and reports
            // "the library never logged", which reads as a product failure.
            std::string RoleLogPath(const char* basePath, LogRole role);

            // THE TWO OPERATIONS EVERY READER ACTUALLY WANTS, so that no caller has to know how
            // many roles there are or how they are spelled.
            //
            // A test asserts that THE RUN said something; which role's thread said it is not
            // what the assertion is about, and guessing wrong turns a real diagnostic into "the
            // child aborted and said nothing". Refusals raised on the apply thread are written
            // under the SERVER role by construction, which is exactly the class of diagnostic
            // these readers exist to check.
            std::string ReadRoleLogs(const char* basePath);
            void TruncateRoleLogs(const char* basePath);
#endif
        } // namespace Debug
    } // namespace MG_Util
} // namespace MobileGL
